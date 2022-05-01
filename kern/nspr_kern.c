#include "ecapture.h"

enum ssl_data_event_type { kSSLRead, kSSLWrite };

struct ssl_data_event_t {
    enum ssl_data_event_type type;
    u64 timestamp_ns;
    u32 pid;
    u32 tid;
    char data[MAX_DATA_SIZE_OPENSSL];
    s32 data_len;
    char comm[TASK_COMM_LEN];
};

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
} nspr_events SEC(".maps");

/***********************************************************
 * Internal structs and definitions
 ***********************************************************/

// Key is thread ID (from bpf_get_current_pid_tgid).
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u64);
    __type(value, const char*);
    __uint(max_entries, 1024);
} active_ssl_read_args_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u64);
    __type(value, const char*);
    __uint(max_entries, 1024);
} active_ssl_write_args_map SEC(".maps");

// BPF programs are limited to a 512-byte stack. We store this value per CPU
// and use it as a heap allocated value.
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __type(key, u32);
    __type(value, struct ssl_data_event_t);
    __uint(max_entries, 1);
} data_buffer_heap SEC(".maps");

/***********************************************************
 * General helper functions
 ***********************************************************/

static __inline struct ssl_data_event_t* create_ssl_data_event(
    u64 current_pid_tgid) {
    u32 kZero = 0;
    struct ssl_data_event_t* event =
        bpf_map_lookup_elem(&data_buffer_heap, &kZero);
    if (event == NULL) {
        return NULL;
    }

    const u32 kMask32b = 0xffffffff;
    event->timestamp_ns = bpf_ktime_get_ns();
    event->pid = current_pid_tgid >> 32;
    event->tid = current_pid_tgid & kMask32b;
    return event;
}

/***********************************************************
 * BPF syscall processing functions
 ***********************************************************/

static int process_SSL_data(struct pt_regs* ctx, u64 id,
                            enum ssl_data_event_type type, const char* buf) {
    int len = (int)PT_REGS_RC(ctx);
    if (len < 0) {
        return 0;
    }

    struct ssl_data_event_t* event = create_ssl_data_event(id);
    if (event == NULL) {
        return 0;
    }

    event->type = type;
    // This is a max function, but it is written in such a way to keep older BPF
    // verifiers happy.
    event->data_len =
        (len < MAX_DATA_SIZE_OPENSSL ? (len & (MAX_DATA_SIZE_OPENSSL - 1))
                                     : MAX_DATA_SIZE_OPENSSL);
    bpf_probe_read(event->data, event->data_len, buf);
    bpf_get_current_comm(&event->comm, sizeof(event->comm));
    bpf_perf_event_output(ctx, &nspr_events, BPF_F_CURRENT_CPU, event,
                          sizeof(struct ssl_data_event_t));
    return 0;
}

/***********************************************************
 * BPF probe function entry-points
 ***********************************************************/
// https://www-archive.mozilla.org/projects/nspr/reference/html/priofnc.html#19250
//
//

SEC("uprobe/PR_Write")
int probe_entry_SSL_write(struct pt_regs* ctx) {
    u64 current_pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = current_pid_tgid >> 32;
    debug_bpf_printk("nspr uprobe/PR_Write pid :%d\n", pid);

#ifndef KERNEL_LESS_5_2
    // if target_ppid is 0 then we target all pids
    if (target_pid != 0 && target_pid != pid) {
        return 0;
    }
#endif

    const char* buf = (const char*)PT_REGS_PARM2(ctx);
    bpf_map_update_elem(&active_ssl_write_args_map, &current_pid_tgid, &buf,
                        BPF_ANY);
    return 0;
}

SEC("uretprobe/PR_Write")
int probe_ret_SSL_write(struct pt_regs* ctx) {
    u64 current_pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = current_pid_tgid >> 32;
    debug_bpf_printk("nspr uretprobe/PR_Write pid :%d\n", pid);

#ifndef KERNEL_LESS_5_2
    // if target_ppid is 0 then we target all pids
    if (target_pid != 0 && target_pid != pid) {
        return 0;
    }
#endif

    const char** buf =
        bpf_map_lookup_elem(&active_ssl_write_args_map, &current_pid_tgid);
    if (buf != NULL) {
        process_SSL_data(ctx, current_pid_tgid, kSSLWrite, *buf);
    }

    bpf_map_delete_elem(&active_ssl_write_args_map, &current_pid_tgid);
    return 0;
}

// Function signature being probed:
// int SSL_read(SSL *s, void *buf, int num)
// ssize_t gnutls_record_recv (gnutls_session session, void * data, size_t
// sizeofdata)

SEC("uprobe/PR_Read")
int probe_entry_SSL_read(struct pt_regs* ctx) {
    u64 current_pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = current_pid_tgid >> 32;
    debug_bpf_printk("nspr uprobe/PR_Read pid :%d\n", pid);

#ifndef KERNEL_LESS_5_2
    // if target_ppid is 0 then we target all pids
    if (target_pid != 0 && target_pid != pid) {
        return 0;
    }
#endif

    const char* buf = (const char*)PT_REGS_PARM2(ctx);
    bpf_map_update_elem(&active_ssl_read_args_map, &current_pid_tgid, &buf,
                        BPF_ANY);
    return 0;
}

SEC("uretprobe/PR_Read")
int probe_ret_SSL_read(struct pt_regs* ctx) {
    u64 current_pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = current_pid_tgid >> 32;
    debug_bpf_printk("nspr uretprobe/PR_Read pid :%d\n", pid);

#ifndef KERNEL_LESS_5_2
    // if target_ppid is 0 then we target all pids
    if (target_pid != 0 && target_pid != pid) {
        return 0;
    }
#endif

    const char** buf =
        bpf_map_lookup_elem(&active_ssl_read_args_map, &current_pid_tgid);
    if (buf != NULL) {
        process_SSL_data(ctx, current_pid_tgid, kSSLRead, *buf);
    }

    bpf_map_delete_elem(&active_ssl_read_args_map, &current_pid_tgid);
    return 0;
}