#include "graywatch/process.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace graywatch {
namespace {

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

}  // namespace

ProcessResult run_process(const std::vector<std::string>& argv,
                          std::chrono::milliseconds timeout,
                          std::size_t maximum_bytes) {
    ProcessResult result;
    if (argv.empty()) return result;
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0) throw std::runtime_error("pipe failed");
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        throw std::runtime_error("pipe failed");
    }
    std::vector<char*> arguments;
    for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) close(fd);
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) dup2(null_fd, STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) close(fd);
        execvp(arguments[0], arguments.data());
        const char message[] = "exec failed\n";
        (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    int out_fd = out_pipe[0];
    int err_fd = err_pipe[0];

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[65536];
    while (out_fd >= 0 || err_fd >= 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            result.timed_out = true;
            break;
        }
        pollfd descriptors[2];
        nfds_t count = 0;
        if (out_fd >= 0) descriptors[count++] = {out_fd, POLLIN, 0};
        if (err_fd >= 0) descriptors[count++] = {err_fd, POLLIN, 0};
        const int ready = poll(descriptors, count, static_cast<int>(remaining.count()));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (nfds_t index = 0; index < count; ++index) {
            if ((descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
            int& fd = descriptors[index].fd == out_fd ? out_fd : err_fd;
            std::string& target = &fd == &out_fd ? result.output : result.error;
            const ssize_t got = read(fd, buffer, sizeof(buffer));
            if (got <= 0) {
                if (got < 0 && errno == EINTR) continue;
                close_fd(fd);
                continue;
            }
            const std::size_t room = maximum_bytes > target.size()
                ? maximum_bytes - target.size() : 0;
            if (static_cast<std::size_t>(got) > room) result.truncated = true;
            target.append(buffer, std::min(room, static_cast<std::size_t>(got)));
        }
    }
    close_fd(out_fd);
    close_fd(err_fd);
    if (result.timed_out) kill(child, SIGKILL);
    int status{};
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!result.timed_out && WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    return result;
}

}  // namespace graywatch
