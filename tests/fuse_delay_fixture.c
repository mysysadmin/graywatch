// Disposable FUSE filesystem for validating Graywatch's FUSE latency evidence.
//
//   fuse_delay_fixture MOUNTPOINT DELAY_MS
//
// Mounts an empty, read-only directory on MOUNTPOINT (root required) and
// answers every LOOKUP with ENOENT after DELAY_MS milliseconds. Unmounting
// (umount MOUNTPOINT) ends the process. Speaks the kernel FUSE protocol
// directly, so it needs only <linux/fuse.h>, not libfuse.

#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fuse_fd = -1;

static void reply(uint64_t unique, int error, const void* body, size_t size) {
    char buffer[sizeof(struct fuse_out_header) + 256];
    struct fuse_out_header header = {
        .len = (uint32_t)(sizeof(header) + (error ? 0 : size)),
        .error = -error,
        .unique = unique,
    };
    memcpy(buffer, &header, sizeof(header));
    if (!error && size) memcpy(buffer + sizeof(header), body, size);
    if (write(fuse_fd, buffer, header.len) < 0 && errno != ENOENT) perror("write");
}

static void sleep_ms(long milliseconds) {
    struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s MOUNTPOINT DELAY_MS\n", argv[0]);
        return 2;
    }
    const long delay_ms = strtol(argv[2], NULL, 10);
    fuse_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (fuse_fd < 0) {
        perror("open /dev/fuse");
        return 1;
    }
    char options[128];
    snprintf(options, sizeof(options), "fd=%d,rootmode=40000,user_id=0,group_id=0,allow_other",
             fuse_fd);
    if (mount("graywatch-test", argv[1], "fuse.graywatch-test",
              MS_NOSUID | MS_NODEV | MS_RDONLY, options) != 0) {
        perror("mount");
        return 1;
    }
    static char buffer[FUSE_MIN_READ_BUFFER + 1024 * 1024];
    for (;;) {
        const ssize_t got = read(fuse_fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR || errno == ENOENT) continue;
            if (errno == ENODEV) return 0;  // unmounted
            perror("read");
            return 1;
        }
        if ((size_t)got < sizeof(struct fuse_in_header)) continue;
        const struct fuse_in_header* in = (const void*)buffer;
        switch (in->opcode) {
            case FUSE_INIT: {
                const struct fuse_init_in* init = (const void*)(in + 1);
                struct fuse_init_out out = {
                    .major = FUSE_KERNEL_VERSION,
                    .minor = init->minor < FUSE_KERNEL_MINOR_VERSION ? init->minor
                                                                     : FUSE_KERNEL_MINOR_VERSION,
                    .max_readahead = init->max_readahead,
                    .max_write = 4096,
                };
                reply(in->unique, 0, &out, sizeof(out));
                break;
            }
            case FUSE_GETATTR: {
                struct fuse_attr_out out = {0};
                out.attr.ino = in->nodeid;
                out.attr.mode = S_IFDIR | 0555;
                out.attr.nlink = 2;
                reply(in->unique, 0, &out, sizeof(out));
                break;
            }
            case FUSE_LOOKUP:
                sleep_ms(delay_ms);
                reply(in->unique, ENOENT, NULL, 0);
                break;
            case FUSE_FORGET:
            case FUSE_BATCH_FORGET:
            case FUSE_INTERRUPT:
                break;  // no reply expected
            case FUSE_DESTROY:
                reply(in->unique, 0, NULL, 0);
                return 0;
            default:
                reply(in->unique, ENOSYS, NULL, 0);
        }
    }
}
