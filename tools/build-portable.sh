#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${GRAYWATCH_PORTABLE_BUILD_DIR:-"$root/build-portable"}
output_dir=${GRAYWATCH_PORTABLE_OUTPUT_DIR:-"$root/dist"}
architecture=$(uname -m)
version=$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)
bundle="graywatch-portable-${version}-linux-${architecture}"
stage="$build_dir/stage/$bundle"

for command in cmake ninja c++ tar sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing required command: $command" >&2
        exit 1
    }
done

cmake -S "$root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGRAYWATCH_STATIC=ON \
    -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure

rm -rf "$build_dir/stage"
mkdir -p "$stage/bin" "$output_dir"
install -m 0755 "$build_dir/graywatchd" "$stage/bin/graywatchd"
install -m 0755 "$build_dir/graywatchctl" "$stage/bin/graywatchctl"
install -m 0755 "$root/tools/collect-graywatch-support.sh" \
    "$stage/collect-graywatch-support.sh"
install -m 0755 "$build_dir/graywatch-report" "$stage/bin/graywatch-report"

# The eBPF helper is included when its toolchain is present (or required with
# GRAYWATCH_PORTABLE_BPF=1; skipped with GRAYWATCH_PORTABLE_BPF=0).
bpf_mode=${GRAYWATCH_PORTABLE_BPF:-auto}
if [ "$bpf_mode" != 0 ]; then
    missing=""
    for command in clang bpftool make; do
        command -v "$command" >/dev/null 2>&1 || missing="$missing $command"
    done
    [ -f "$root/third_party/libbpf/src/Makefile" ] || missing="$missing libbpf-submodule"
    if [ -z "$missing" ]; then
        make -C "$root/bpf" clean >/dev/null
        make -C "$root/bpf" STATIC=1
        install -m 0755 "$root/bpf/.output/graywatch-bpf" "$stage/bin/graywatch-bpf"
    elif [ "$bpf_mode" = 1 ]; then
        echo "cannot build graywatch-bpf, missing:$missing" >&2
        exit 1
    else
        echo "skipping graywatch-bpf, missing:$missing" >&2
    fi
fi
install -m 0644 "$root/portable/README.txt" "$stage/README.txt"

for binary in graywatchd graywatchctl graywatch-report; do
    if ldd "$stage/bin/$binary" 2>&1 | grep -qv "not a dynamic executable"; then
        echo "$binary is not fully static" >&2
        exit 1
    fi
done
if [ -x "$stage/bin/graywatch-bpf" ]; then
    unexpected=$(ldd "$stage/bin/graywatch-bpf" | awk '{print $1}' |
        grep -Ev '^(linux-vdso\.so|/lib64/ld-linux|libc\.so|libm\.so|libelf\.so|libz\.so|libzstd\.so)' || true)
    if [ -n "$unexpected" ]; then
        echo "graywatch-bpf has unexpected dynamic dependencies: $unexpected" >&2
        exit 1
    fi
fi

(cd "$stage" && sha256sum bin/* > SHA256SUMS)
tar -C "$build_dir/stage" -czf "$output_dir/$bundle.tar.gz" "$bundle"
(cd "$output_dir" && sha256sum "$bundle.tar.gz" > "$bundle.tar.gz.sha256")
echo "$output_dir/$bundle.tar.gz"
