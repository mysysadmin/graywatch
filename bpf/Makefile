OUTPUT := .output
LIBBPF_SRC := ../third_party/libbpf/src
LIBBPF_DEST := $(abspath $(OUTPUT)/root)
LIBBPF_A := $(LIBBPF_DEST)/usr/lib64/libbpf.a
INCLUDES := -I$(LIBBPF_DEST)/usr/include -I$(OUTPUT)

.PHONY: all clean
all: $(OUTPUT)/graywatch-biolatency

$(OUTPUT):
	mkdir -p $@

$(LIBBPF_A): | $(OUTPUT)
	$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1 OBJDIR=$(abspath $(OUTPUT)/libbpf) \
		DESTDIR=$(LIBBPF_DEST) PREFIX=/usr LIBDIR=/usr/lib64 install

$(OUTPUT)/biolatency.bpf.o: biolatency.bpf.c vmlinux_min.h $(LIBBPF_A) | $(OUTPUT)
	clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -DGRAYWATCH_BPF $(INCLUDES) -c $< -o $@

$(OUTPUT)/biolatency.skel.h: $(OUTPUT)/biolatency.bpf.o
	bpftool gen skeleton $< > $@

$(OUTPUT)/graywatch-biolatency: biolatency.cpp $(OUTPUT)/biolatency.skel.h $(LIBBPF_A)
	$(CXX) -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror $(INCLUDES) $< \
		$(LIBBPF_A) -lelf -lz -o $@

clean:
	rm -rf $(OUTPUT)
