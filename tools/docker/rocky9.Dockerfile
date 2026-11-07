# Builder for the portable bundle. Rocky 9 (glibc 2.34) is the oldest
# supported baseline: graywatch-bpf links glibc dynamically, so it runs on
# glibc 2.34 or later. The other binaries are fully static.
FROM rockylinux:9
RUN dnf -y install dnf-plugins-core && dnf config-manager --set-enabled crb && \
    dnf -y install gcc-c++ make cmake ninja-build git tar gzip findutils which \
      glibc-static libstdc++-static clang llvm bpftool \
      elfutils-libelf-devel zlib-devel libzstd-devel && \
    dnf clean all
