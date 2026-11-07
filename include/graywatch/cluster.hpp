#pragma once

namespace graywatch {

// `graywatchctl cluster HOST... [--user USER] [--timeout SECONDS]
// [--remote-command PATH] [--socket PATH] [--json]`: query graywatchd on each
// host over SSH in parallel and print a one-line summary per host. Returns 0
// when every host answered, 1 otherwise.
int run_cluster(int argc, char** argv, int first_argument);

}  // namespace graywatch
