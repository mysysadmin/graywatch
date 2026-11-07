#include "vmlinux_min.h"
#include "biolatency.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

struct request_start {
    __u64 timestamp_ns;
    __u32 dev;
    __u32 operation;
    __u32 bytes;
};

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

static __always_inline struct io_stats *get_stats(struct io_key *key) {
    struct io_stats zero = {};
    struct io_stats *value = bpf_map_lookup_elem(&statistics, key);
    if (value) return value;
    bpf_map_update_elem(&statistics, key, &zero, BPF_NOEXIST);
    return bpf_map_lookup_elem(&statistics, key);
}

SEC("fentry/blk_mq_start_request")
int BPF_PROG(account_start, struct request___local *request) {
    const __u64 pointer = (__u64)request;
    const unsigned int flags = BPF_CORE_READ(request, cmd_flags);
    const struct request_start start = {
        .timestamp_ns = bpf_ktime_get_ns(),
        .dev = request_device(request),
        .operation = flags & 0xff,
        .bytes = BPF_CORE_READ(request, __data_len),
    };
    bpf_map_update_elem(&starts, &pointer, &start, BPF_ANY);
    return 0;
}

SEC("fentry/blk_mq_end_request")
int BPF_PROG(account_done, struct request___local *request, __u8 error) {
    const __u64 pointer = (__u64)request;
    const struct request_start *start = bpf_map_lookup_elem(&starts, &pointer);
    if (!start) return 0;
    const __u64 latency_ns = bpf_ktime_get_ns() - start->timestamp_ns;
    const struct io_key key = {.dev = start->dev, .operation = start->operation};
    struct io_stats *stats = get_stats((struct io_key *)&key);
    if (stats) {
        const __u64 latency_us = latency_ns / 1000;
        __u32 bucket = latency_us == 0 ? 0 : 63 - __builtin_clzll(latency_us);
        if (bucket >= GRAYWATCH_LATENCY_BUCKETS) bucket = GRAYWATCH_LATENCY_BUCKETS - 1;
        __sync_fetch_and_add(&stats->count, 1);
        __sync_fetch_and_add(&stats->bytes, start->bytes);
        __sync_fetch_and_add(&stats->total_latency_ns, latency_ns);
        __sync_fetch_and_add(&stats->buckets[bucket], 1);
        if (error) __sync_fetch_and_add(&stats->errors, 1);
    }
    bpf_map_delete_elem(&starts, &pointer);
    return 0;
}

