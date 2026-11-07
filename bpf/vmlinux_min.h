#ifndef GRAYWATCH_VMLINUX_MIN_H
#define GRAYWATCH_VMLINUX_MIN_H

typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef signed int __s32;
typedef signed long long __s64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u32 __wsum;

typedef __u32 dev_t;

struct gendisk___local {
    int major;
    int first_minor;
} __attribute__((preserve_access_index));

struct request_queue___local {
    struct gendisk___local *disk;
} __attribute__((preserve_access_index));

struct request___local {
    struct request_queue___local *q;
    unsigned int cmd_flags;
    unsigned int __data_len;
} __attribute__((preserve_access_index));

struct trace_event_raw_block_rq_completion___local {
    unsigned long long __ignore[1];
    dev_t dev;
    unsigned long long sector;
    unsigned int nr_sector;
    int error;
} __attribute__((preserve_access_index));

#endif
