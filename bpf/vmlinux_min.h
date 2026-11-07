#ifndef GRAYWATCH_VMLINUX_MIN_H
#define GRAYWATCH_VMLINUX_MIN_H

// Minimal, hand-written kernel type definitions. Every struct carries
// preserve_access_index, so libbpf CO-RE relocates each field access against
// the running kernel's BTF (including module BTF such as fuse). Only fields the
// programs read are declared; their order and offsets here do not matter.

typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef signed char __s8;
typedef signed short __s16;
typedef signed int __s32;
typedef signed long long __s64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u32 __wsum;

typedef __u32 dev_t;

enum {
    BPF_MAP_TYPE_HASH = 1,
    BPF_MAP_TYPE_ARRAY = 2,
    BPF_MAP_TYPE_PERCPU_ARRAY = 6,
    BPF_MAP_TYPE_STACK_TRACE = 7,
};

enum {
    BPF_ANY = 0,
    BPF_NOEXIST = 1,
};

// Block layer.
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

struct bpf_raw_tracepoint_args {
    __u64 args[0];
};

// Scheduler. Linux 5.14 renamed task_struct.state to __state and changed its
// type; both flavors are declared and selected with bpf_core_field_exists.
struct kernfs_node___local {
    __u64 id;
} __attribute__((preserve_access_index));

struct cgroup___local {
    struct kernfs_node___local *kn;
} __attribute__((preserve_access_index));

struct css_set___local {
    struct cgroup___local *dfl_cgrp;
} __attribute__((preserve_access_index));

struct task_struct___local {
    int pid;
    int tgid;
    char comm[16];
    struct css_set___local *cgroups;
} __attribute__((preserve_access_index));

struct task_struct___new {
    unsigned int __state;
} __attribute__((preserve_access_index));

struct task_struct___old {
    long state;
} __attribute__((preserve_access_index));

// FUSE (module BTF). fuse_simple_request(struct fuse_mount *, struct fuse_args *)
// on 5.10+; __fuse_simple_request(struct mnt_idmap *, fm, args) on 6.x.
struct super_block___local {
    dev_t s_dev;
} __attribute__((preserve_access_index));

struct fuse_mount___local {
    struct super_block___local *sb;
} __attribute__((preserve_access_index));

struct fuse_args___local {
    __u32 opcode;
} __attribute__((preserve_access_index));

// Networking.
struct in6_addr___local {
    __u8 bytes[16];
} __attribute__((preserve_access_index));

struct sock_common___local {
    __be32 skc_daddr;
    __be32 skc_rcv_saddr;
    __be16 skc_dport;
    __u16 skc_num;
    unsigned short skc_family;
    unsigned char skc_state;
    struct in6_addr___local skc_v6_daddr;
    struct in6_addr___local skc_v6_rcv_saddr;
} __attribute__((preserve_access_index));

struct sock___local {
    struct sock_common___local __sk_common;
} __attribute__((preserve_access_index));

#endif
