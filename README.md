# Graywatch

Graywatch is an experimental local Linux degradation detector. It observes the
quality of a process's execution environment and exposes evidence-backed health
states to applications.

The first milestone reads Linux Pressure Stall Information (PSI). This gives us
a useful, unprivileged signal and lets us validate the build and deployment
workflow before adding eBPF collectors.

## Build

Requirements:

- Linux with PSI enabled (`/proc/pressure`)
- CMake 3.20 or newer
- A C++20 compiler

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Read the current PSI snapshot:

```sh
./build/graywatchctl psi
```

Example output:

```text
cpu some avg10=0.00 avg60=0.00 avg300=0.00 total_us=1234
memory some avg10=0.00 avg60=0.00 avg300=0.00 total_us=5678
memory full avg10=0.00 avg60=0.00 avg300=0.00 total_us=42
io some avg10=0.00 avg60=0.00 avg300=0.00 total_us=9012
io full avg10=0.00 avg60=0.00 avg300=0.00 total_us=321
```

## Why start with PSI?

PSI measures time during which tasks cannot make progress because CPU, memory,
or I/O is unavailable. It is already provided by the Linux kernel, requires no
privileged probe, and gives Graywatch a sound first input while its event model,
baseline learning, and local API are developed.
