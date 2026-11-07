#ifndef GRAYWATCH_BPF_SHARED_H
#define GRAYWATCH_BPF_SHARED_H

// Types shared between graywatch.bpf.c and graywatch-bpf.cpp.

#ifndef GRAYWATCH_BPF
#include <linux/types.h>
#endif

#define GW_BUCKETS 32       // log2 microsecond buckets: bucket i holds [2^i, 2^(i+1)) us
#define GW_COMM_LEN 16

// Indexes into the per-CPU drop counters (map update failures, i.e. full maps).
enum gw_collector {
    GW_FUSE = 0,
    GW_BLOCK = 1,
    GW_OFFCPU = 2,
    GW_RUNQUEUE = 3,
    GW_TCP = 4,
    GW_RECLAIM = 5,
    GW_COLLECTORS = 6,
};

struct gw_hist {
    __u64 count;
    __u64 total_ns;
    __u64 max_ns;
    __u64 buckets[GW_BUCKETS];
};

// 1. FUSE: synchronous request latency per (superblock device, opcode).
struct fuse_key {
    __u32 dev;      // kernel dev_t encoding: major << 20 | minor
    __u32 opcode;
};

struct fuse_stats {
    struct gw_hist latency;
    __u64 errors;   // negative replies except ENOENT, ENODATA, ERANGE, ENOSYS, EOPNOTSUPP
};

struct fuse_start {
    __u64 start_ns;
    __u32 dev;
    __u32 opcode;
};

// 2. Block: queue (insert -> dispatch) and service (dispatch -> completion).
struct io_key {
    __u32 dev;
    __u32 operation;
};

struct io_stats {
    struct gw_hist service;
    struct gw_hist queue;
    __u64 bytes;
    __u64 errors;
    __u64 direct_issue;   // dispatched without passing block_rq_insert
};

struct request_start {
    __u64 timestamp_ns;
    __u32 dev;
    __u32 operation;
    __u32 bytes;
    __u32 pad;
};

// 3. Off-CPU uninterruptible sleep per (process, kernel stack).
struct offcpu_key {
    __u32 tgid;
    __s32 stack_id;
};

struct offcpu_stats {
    struct gw_hist blocked;
    char comm[GW_COMM_LEN];
};

struct blocked_task {
    __u64 start_ns;
    __u32 tgid;
    __s32 stack_id;
    char comm[GW_COMM_LEN];
};

// 4. Run-queue latency per cgroup v2 id: struct gw_hist keyed by __u64.

// 5. TCP retransmissions per connection.
struct tcp_key {
    __u8 saddr[16];
    __u8 daddr[16];
    __u16 family;
    __u16 sport;   // host byte order
    __u16 dport;   // host byte order
    __u16 pad;
};

struct tcp_stats {
    __u64 retransmits;
    __u32 state;
    __u32 pad;
};

// 6. Memory reclaim stalls per (process, kind, cgroup).
enum gw_reclaim_kind {
    GW_RECLAIM_DIRECT = 0,
    GW_RECLAIM_MEMCG = 1,
};

struct reclaim_key {
    __u64 cgroup_id;
    __u32 tgid;
    __u32 kind;
};

struct reclaim_stats {
    struct gw_hist stall;
    __u64 reclaimed_pages;
    char comm[GW_COMM_LEN];
};

struct reclaim_start {
    __u64 start_ns;
    __u64 cgroup_id;
};

#endif
