# Builds graywatch-bpf, the optional privileged eBPF collector.
#   make -C bpf            dynamic build (needs libelf and zlib shared libraries)
#   make -C bpf STATIC=1   portable build: libbpf, libstdc++ and libgcc linked
#                          statically; only glibc, libelf.so.1 and libz.so.1
#                          remain dynamic (no static libelf on RHEL-family
#                          distributions)
OUTPUT := .output
LIBBPF_SRC := ../third_party/libbpf/src
LIBBPF_DEST := $(abspath $(OUTPUT)/root)
LIBBPF_A := $(LIBBPF_DEST)/usr/lib64/libbpf.a
INCLUDES := -I$(LIBBPF_DEST)/usr/include -I$(OUTPUT)
ARCH := $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
CXX ?= c++

ifeq ($(STATIC),1)
LINK := -static-libstdc++ -static-libgcc -lelf -lz
else
LINK := -lelf -lz
endif

.PHONY: all clean
all: $(OUTPUT)/graywatch-bpf

$(OUTPUT):
	mkdir -p $@

$(LIBBPF_A): | $(OUTPUT)
	$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1 OBJDIR=$(abspath $(OUTPUT)/libbpf) \
		DESTDIR=$(LIBBPF_DEST) PREFIX=/usr LIBDIR=/usr/lib64 install

$(OUTPUT)/graywatch.bpf.o: graywatch.bpf.c graywatch.h vmlinux_min.h $(LIBBPF_A) | $(OUTPUT)
	clang -g -O2 -Wall -Werror -target bpf -D__TARGET_ARCH_$(ARCH) -DGRAYWATCH_BPF \
		$(INCLUDES) -c $< -o $@

$(OUTPUT)/graywatch.skel.h: $(OUTPUT)/graywatch.bpf.o
	bpftool gen skeleton $< name graywatch_bpf > $@

$(OUTPUT)/graywatch-bpf: graywatch-bpf.cpp graywatch.h $(OUTPUT)/graywatch.skel.h $(LIBBPF_A)
	$(CXX) -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror $(INCLUDES) $< \
		$(LIBBPF_A) $(LINK) -o $@

clean:
	rm -rf $(OUTPUT)
