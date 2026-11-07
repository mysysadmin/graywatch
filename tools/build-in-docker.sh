#!/bin/sh
# Build the portable bundle inside a container so the result does not depend
# on the host's glibc or toolchain.
#
#   tools/build-in-docker.sh [TARGET]      (default TARGET: rocky9)
#
# TARGET selects tools/docker/TARGET.Dockerfile. The bundle is written to
# dist/TARGET/. GRAYWATCH_JOBS sets build parallelism (default: nproc).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=${1:-rocky9}
dockerfile="$root/tools/docker/$target.Dockerfile"
image="graywatch-build:$target"
jobs=${GRAYWATCH_JOBS:-$(nproc)}

[ -f "$dockerfile" ] || {
    echo "unknown target $target (no $dockerfile)" >&2
    exit 2
}
[ -f "$root/third_party/libbpf/src/Makefile" ] ||
    git -C "$root" submodule update --init --recursive

# Rebuilds only when the Dockerfile changed; layers are cached otherwise.
docker build -q -t "$image" -f "$dockerfile" "$root/tools/docker" >/dev/null

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e CMAKE_BUILD_PARALLEL_LEVEL="$jobs" -e MAKEFLAGS="-j$jobs" \
    -e GRAYWATCH_PORTABLE_BPF="${GRAYWATCH_PORTABLE_BPF:-1}" \
    -e GRAYWATCH_PORTABLE_BUILD_DIR="/src/build-$target" \
    -e GRAYWATCH_PORTABLE_OUTPUT_DIR="/src/dist/$target" \
    -v "$root:/src" -w /src "$image" tools/build-portable.sh
