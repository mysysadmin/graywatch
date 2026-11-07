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
install -m 0644 "$root/portable/README.txt" "$stage/README.txt"

if ldd "$stage/bin/graywatchd" 2>&1 | grep -qv "not a dynamic executable"; then
    echo "graywatchd is not fully static" >&2
    exit 1
fi
if ldd "$stage/bin/graywatchctl" 2>&1 | grep -qv "not a dynamic executable"; then
    echo "graywatchctl is not fully static" >&2
    exit 1
fi

(cd "$stage" && sha256sum bin/graywatchd bin/graywatchctl > SHA256SUMS)
tar -C "$build_dir/stage" -czf "$output_dir/$bundle.tar.gz" "$bundle"
sha256sum "$output_dir/$bundle.tar.gz" > "$output_dir/$bundle.tar.gz.sha256"
echo "$output_dir/$bundle.tar.gz"
