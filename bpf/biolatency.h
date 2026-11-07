#ifndef GRAYWATCH_BIOLATENCY_H
#define GRAYWATCH_BIOLATENCY_H

#ifndef GRAYWATCH_BPF
#include <linux/types.h>
#endif

#define GRAYWATCH_LATENCY_BUCKETS 32

struct io_key {
    __u32 dev;
    __u32 operation;
};

struct io_stats {
    __u64 count;
    __u64 bytes;
    __u64 total_latency_ns;
    __u64 buckets[GRAYWATCH_LATENCY_BUCKETS];
    __u64 errors;
};

#endif
