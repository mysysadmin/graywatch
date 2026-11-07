// Graywatch eBPF collectors. Each collector is a group of programs that the
// loader enables only when its hooks exist in the running kernel's BTF, so one
// missing tracepoint or module disables that collector, not the whole helper.
//
// All maps hold cumulative aggregates; user space computes interval deltas.

#include "vmlinux_min.h"
#include "graywatch.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

#define TASK_RUNNING 0x0
#define TASK_UNINTERRUPTIBLE 0x2
#define TASK_NOLOAD 0x400
#define ENOENT 2
#define ERANGE 34
#define ENOSYS 38
#define ENODATA 61
#define EOPNOTSUPP 95
#define AF_INET 2
#define AF_INET6 10
#define STACK_DEPTH 32

// All-zero initial value for new aggregate entries; the largest value
// (io_stats) is too big for the 512-byte BPF stack.
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct io_stats);
} zeroes SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, GW_COLLECTORS);
    __type(key, __u32);
    __type(value, __u64);
} drops SEC(".maps");

static __always_inline void count_drop(__u32 collector) {
    __u64 *value = bpf_map_lookup_elem(&drops, &collector);
    if (value) *value += 1;
}

static __always_inline void *lookup_or_init(void *map, const void *key, __u32 collector) {
    void *value = bpf_map_lookup_elem(map, key);
    if (value) return value;
    __u32 zero_key = 0;
    const void *zero = bpf_map_lookup_elem(&zeroes, &zero_key);
    if (!zero) return 0;
    const long result = bpf_map_update_elem(map, key, zero, BPF_NOEXIST);
    if (result != 0 && result != -17 /* EEXIST: raced with another CPU */) {
        count_drop(collector);
        return 0;
    }
    return bpf_map_lookup_elem(map, key);
}

static __always_inline void hist_add(struct gw_hist *hist, __u64 ns) {
    const __u64 us = ns / 1000;
    __u32 bucket = us == 0 ? 0 : 63 - __builtin_clzll(us);
    if (bucket >= GW_BUCKETS) bucket = GW_BUCKETS - 1;
    __sync_fetch_and_add(&hist->count, 1);
    __sync_fetch_and_add(&hist->total_ns, ns);
    __sync_fetch_and_add(&hist->buckets[bucket], 1);
    // The maximum is updated without atomics; a lost race only understates it.
    if (ns > hist->max_ns) hist->max_ns = ns;
}

static __always_inline void save(void *map, const void *key, const void *value,
                                 __u64 flags, __u32 collector) {
    if (bpf_map_update_elem(map, key, value, flags) != 0) count_drop(collector);
}

static __always_inline long task_state(struct task_struct___local *task) {
    struct task_struct___new *modern = (void *)task;
    if (bpf_core_field_exists(modern->__state)) return BPF_CORE_READ(modern, __state);
    struct task_struct___old *legacy = (void *)task;
    return BPF_CORE_READ(legacy, state);
}

// ---------------------------------------------------------------------------
// 1. FUSE request latency
// ---------------------------------------------------------------------------

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, __u32);
    __type(value, struct fuse_start);
} fuse_starts SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, struct fuse_key);
    __type(value, struct fuse_stats);
} fuse_statistics SEC(".maps");

static __always_inline int fuse_begin(struct fuse_mount___local *mount,
                                      struct fuse_args___local *args) {
    const __u32 tid = (__u32)bpf_get_current_pid_tgid();
    const struct fuse_start start = {
        .start_ns = bpf_ktime_get_ns(),
        .dev = BPF_CORE_READ(mount, sb, s_dev),
        .opcode = BPF_CORE_READ(args, opcode),
    };
    save(&fuse_starts, &tid, &start, BPF_ANY, GW_FUSE);
    return 0;
}

static __always_inline int fuse_end(long result) {
    const __u32 tid = (__u32)bpf_get_current_pid_tgid();
    const struct fuse_start *start = bpf_map_lookup_elem(&fuse_starts, &tid);
    if (!start) return 0;
    const __u64 elapsed = bpf_ktime_get_ns() - start->start_ns;
    const struct fuse_key key = {.dev = start->dev, .opcode = start->opcode};
    struct fuse_stats *stats = lookup_or_init(&fuse_statistics, &key, GW_FUSE);
    if (stats) {
        hist_add(&stats->latency, elapsed);
        // Expected negative answers: missing names and attributes, size
        // probes, and operations the filesystem does not implement.
        if (result < 0 && result != -ENOENT && result != -ENODATA && result != -ERANGE &&
            result != -ENOSYS && result != -EOPNOTSUPP) {
            __sync_fetch_and_add(&stats->errors, 1);
        }
    }
    bpf_map_delete_elem(&fuse_starts, &tid);
    return 0;
}

SEC("fentry/fuse_simple_request")
int fuse_enter(unsigned long long *ctx) {
    return fuse_begin((void *)ctx[0], (void *)ctx[1]);
}

SEC("fexit/fuse_simple_request")
int fuse_exit(unsigned long long *ctx) {
    return fuse_end((long)ctx[2]);
}

SEC("fentry/__fuse_simple_request")
int fuse_enter_idmap(unsigned long long *ctx) {
    return fuse_begin((void *)ctx[1], (void *)ctx[2]);
}

SEC("fexit/__fuse_simple_request")
int fuse_exit_idmap(unsigned long long *ctx) {
    return fuse_end((long)ctx[3]);
}

// ---------------------------------------------------------------------------
// 2. Block queue and service latency
// ---------------------------------------------------------------------------

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u64);
    __type(value, __u64);
} inserts SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u64);
    __type(value, struct request_start);
} starts SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, struct io_key);
    __type(value, struct io_stats);
} statistics SEC(".maps");

static __always_inline __u32 request_device(struct request___local *request) {
    struct request_queue___local *queue = BPF_CORE_READ(request, q);
    struct gendisk___local *disk = BPF_CORE_READ(queue, disk);
    const __u32 major = BPF_CORE_READ(disk, major);
    const __u32 minor = BPF_CORE_READ(disk, first_minor);
    return (major << 20) | minor;
}

SEC("raw_tp/block_rq_insert")
int block_insert(struct bpf_raw_tracepoint_args *context) {
    const __u64 pointer = context->args[0];
    const __u64 now = bpf_ktime_get_ns();
    save(&inserts, &pointer, &now, BPF_ANY, GW_BLOCK);
    return 0;
}

SEC("fentry/blk_mq_start_request")
int BPF_PROG(account_start, struct request___local *request) {
    const __u64 pointer = (__u64)request;
    const __u64 now = bpf_ktime_get_ns();
    const unsigned int flags = BPF_CORE_READ(request, cmd_flags);
    const struct request_start start = {
        .timestamp_ns = now,
        .dev = request_device(request),
        .operation = flags & 0xff,
        .bytes = BPF_CORE_READ(request, __data_len),
    };
    const struct io_key key = {.dev = start.dev, .operation = start.operation};
    struct io_stats *stats = lookup_or_init(&statistics, &key, GW_BLOCK);
    const __u64 *inserted = bpf_map_lookup_elem(&inserts, &pointer);
    if (stats) {
        if (inserted) hist_add(&stats->queue, now - *inserted);
        else __sync_fetch_and_add(&stats->direct_issue, 1);
    }
    if (inserted) bpf_map_delete_elem(&inserts, &pointer);
    save(&starts, &pointer, &start, BPF_ANY, GW_BLOCK);
    return 0;
}

SEC("raw_tp/block_rq_complete")
int account_done(struct bpf_raw_tracepoint_args *context) {
    const __u64 pointer = context->args[0];
    const __u8 error = (__u8)context->args[1];
    const struct request_start *start = bpf_map_lookup_elem(&starts, &pointer);
    if (!start) return 0;
    const __u64 elapsed = bpf_ktime_get_ns() - start->timestamp_ns;
    const struct io_key key = {.dev = start->dev, .operation = start->operation};
    struct io_stats *stats = lookup_or_init(&statistics, &key, GW_BLOCK);
    if (stats) {
        hist_add(&stats->service, elapsed);
        __sync_fetch_and_add(&stats->bytes, start->bytes);
        if (error) __sync_fetch_and_add(&stats->errors, 1);
    }
    bpf_map_delete_elem(&starts, &pointer);
    return 0;
}

// ---------------------------------------------------------------------------
// 3. Uninterruptible (D-state) off-CPU time with kernel stacks
// ---------------------------------------------------------------------------

struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(max_entries, 16384);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, STACK_DEPTH * sizeof(__u64));
} stacks SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, struct blocked_task);
} blocked SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, struct offcpu_key);
    __type(value, struct offcpu_stats);
} offcpu_statistics SEC(".maps");

// sched_switch(bool preempt, struct task_struct *prev, struct task_struct *next
// [, unsigned int prev_state]) runs in prev's context, so the current kernel
// stack is the stack prev is going to sleep in.
SEC("tp_btf/sched_switch")
int offcpu_switch(unsigned long long *ctx) {
    if (ctx[0]) return 0;  // preempted, still runnable
    struct task_struct___local *prev = (void *)ctx[1];
    const long state = task_state(prev);
    if (!(state & TASK_UNINTERRUPTIBLE) || (state & TASK_NOLOAD)) return 0;
    const __u32 tid = BPF_CORE_READ(prev, pid);
    if (tid == 0) return 0;
    struct blocked_task task = {
        .start_ns = bpf_ktime_get_ns(),
        .tgid = BPF_CORE_READ(prev, tgid),
        .stack_id = bpf_get_stackid(ctx, &stacks, 0),
    };
    bpf_get_current_comm(task.comm, sizeof(task.comm));
    save(&blocked, &tid, &task, BPF_ANY, GW_OFFCPU);
    return 0;
}

SEC("tp_btf/sched_wakeup")
int offcpu_wakeup(unsigned long long *ctx) {
    struct task_struct___local *task = (void *)ctx[0];
    const __u32 tid = BPF_CORE_READ(task, pid);
    const struct blocked_task *entry = bpf_map_lookup_elem(&blocked, &tid);
    if (!entry) return 0;
    const __u64 elapsed = bpf_ktime_get_ns() - entry->start_ns;
    const struct offcpu_key key = {.tgid = entry->tgid, .stack_id = entry->stack_id};
    struct offcpu_stats *stats = bpf_map_lookup_elem(&offcpu_statistics, &key);
    if (!stats) {
        stats = lookup_or_init(&offcpu_statistics, &key, GW_OFFCPU);
        if (stats) __builtin_memcpy(stats->comm, entry->comm, GW_COMM_LEN);
    }
    if (stats) hist_add(&stats->blocked, elapsed);
    bpf_map_delete_elem(&blocked, &tid);
    return 0;
}

// ---------------------------------------------------------------------------
// 4. Run-queue latency per cgroup
// ---------------------------------------------------------------------------

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 131072);
    __type(key, __u32);
    __type(value, __u64);
} runnable_since SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8192);
    __type(key, __u64);
    __type(value, struct gw_hist);
} runqueue_statistics SEC(".maps");

static __always_inline void mark_runnable(struct task_struct___local *task) {
    const __u32 tid = BPF_CORE_READ(task, pid);
    if (tid == 0) return;
    const __u64 now = bpf_ktime_get_ns();
    save(&runnable_since, &tid, &now, BPF_ANY, GW_RUNQUEUE);
}

SEC("tp_btf/sched_wakeup")
int runqueue_wakeup(unsigned long long *ctx) {
    mark_runnable((void *)ctx[0]);
    return 0;
}

SEC("tp_btf/sched_wakeup_new")
int runqueue_wakeup_new(unsigned long long *ctx) {
    mark_runnable((void *)ctx[0]);
    return 0;
}

SEC("tp_btf/sched_switch")
int runqueue_switch(unsigned long long *ctx) {
    struct task_struct___local *prev = (void *)ctx[1];
    struct task_struct___local *next = (void *)ctx[2];
    // A preempted task, or one switched out while still TASK_RUNNING, goes
    // straight back to the run queue.
    if (ctx[0] || task_state(prev) == TASK_RUNNING) mark_runnable(prev);
    const __u32 tid = BPF_CORE_READ(next, pid);
    if (tid == 0) return 0;
    const __u64 *since = bpf_map_lookup_elem(&runnable_since, &tid);
    if (!since) return 0;
    const __u64 waited = bpf_ktime_get_ns() - *since;
    const __u64 cgroup = BPF_CORE_READ(next, cgroups, dfl_cgrp, kn, id);
    struct gw_hist *stats = lookup_or_init(&runqueue_statistics, &cgroup, GW_RUNQUEUE);
    if (stats) hist_add(stats, waited);
    bpf_map_delete_elem(&runnable_since, &tid);
    return 0;
}

// ---------------------------------------------------------------------------
// 5. TCP retransmissions per connection
// ---------------------------------------------------------------------------

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, struct tcp_key);
    __type(value, struct tcp_stats);
} tcp_statistics SEC(".maps");

SEC("tp_btf/tcp_retransmit_skb")
int tcp_retransmit(unsigned long long *ctx) {
    struct sock___local *sock = (void *)ctx[0];
    struct tcp_key key = {};
    key.family = BPF_CORE_READ(sock, __sk_common.skc_family);
    if (key.family == AF_INET) {
        const __be32 source = BPF_CORE_READ(sock, __sk_common.skc_rcv_saddr);
        const __be32 destination = BPF_CORE_READ(sock, __sk_common.skc_daddr);
        __builtin_memcpy(key.saddr, &source, 4);
        __builtin_memcpy(key.daddr, &destination, 4);
    } else if (key.family == AF_INET6) {
        BPF_CORE_READ_INTO(&key.saddr, sock, __sk_common.skc_v6_rcv_saddr);
        BPF_CORE_READ_INTO(&key.daddr, sock, __sk_common.skc_v6_daddr);
    } else {
        return 0;
    }
    key.sport = BPF_CORE_READ(sock, __sk_common.skc_num);
    key.dport = bpf_ntohs(BPF_CORE_READ(sock, __sk_common.skc_dport));
    struct tcp_stats *stats = bpf_map_lookup_elem(&tcp_statistics, &key);
    if (!stats) {
        const struct tcp_stats initial = {};
        if (bpf_map_update_elem(&tcp_statistics, &key, &initial, BPF_NOEXIST) != 0 &&
            !bpf_map_lookup_elem(&tcp_statistics, &key)) {
            count_drop(GW_TCP);
            return 0;
        }
        stats = bpf_map_lookup_elem(&tcp_statistics, &key);
        if (!stats) return 0;
    }
    __sync_fetch_and_add(&stats->retransmits, 1);
    stats->state = BPF_CORE_READ(sock, __sk_common.skc_state);
    return 0;
}

// ---------------------------------------------------------------------------
// 6. Memory reclaim stalls (global direct reclaim and memcg limit reclaim)
// ---------------------------------------------------------------------------

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);
    __type(value, struct reclaim_start);
} reclaim_starts SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, struct reclaim_key);
    __type(value, struct reclaim_stats);
} reclaim_statistics SEC(".maps");

static __always_inline __u64 reclaim_slot(__u32 kind) {
    return ((__u64)(__u32)bpf_get_current_pid_tgid() << 1) | kind;
}

static __always_inline int reclaim_begin(__u32 kind) {
    const __u64 slot = reclaim_slot(kind);
    const struct reclaim_start start = {
        .start_ns = bpf_ktime_get_ns(),
        .cgroup_id = bpf_get_current_cgroup_id(),
    };
    save(&reclaim_starts, &slot, &start, BPF_ANY, GW_RECLAIM);
    return 0;
}

static __always_inline int reclaim_end(__u32 kind, __u64 reclaimed) {
    const __u64 slot = reclaim_slot(kind);
    const struct reclaim_start *start = bpf_map_lookup_elem(&reclaim_starts, &slot);
    if (!start) return 0;
    const __u64 elapsed = bpf_ktime_get_ns() - start->start_ns;
    const struct reclaim_key key = {
        .cgroup_id = start->cgroup_id,
        .tgid = bpf_get_current_pid_tgid() >> 32,
        .kind = kind,
    };
    struct reclaim_stats *stats = bpf_map_lookup_elem(&reclaim_statistics, &key);
    if (!stats) {
        stats = lookup_or_init(&reclaim_statistics, &key, GW_RECLAIM);
        if (stats) bpf_get_current_comm(stats->comm, sizeof(stats->comm));
    }
    if (stats) {
        hist_add(&stats->stall, elapsed);
        __sync_fetch_and_add(&stats->reclaimed_pages, reclaimed);
    }
    bpf_map_delete_elem(&reclaim_starts, &slot);
    return 0;
}

SEC("tp_btf/mm_vmscan_direct_reclaim_begin")
int reclaim_direct_begin(unsigned long long *ctx) {
    (void)ctx;
    return reclaim_begin(GW_RECLAIM_DIRECT);
}

SEC("tp_btf/mm_vmscan_direct_reclaim_end")
int reclaim_direct_end(unsigned long long *ctx) {
    return reclaim_end(GW_RECLAIM_DIRECT, ctx[0]);
}

SEC("tp_btf/mm_vmscan_memcg_reclaim_begin")
int reclaim_memcg_begin(unsigned long long *ctx) {
    (void)ctx;
    return reclaim_begin(GW_RECLAIM_MEMCG);
}

SEC("tp_btf/mm_vmscan_memcg_reclaim_end")
int reclaim_memcg_end(unsigned long long *ctx) {
    return reclaim_end(GW_RECLAIM_MEMCG, ctx[0]);
}
