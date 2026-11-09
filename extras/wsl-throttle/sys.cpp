#include "sys.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace sys {
namespace {

// Generous enough for a docker inspect of a few hundred containers, still
// bounded so a runaway command cannot exhaust memory.
constexpr size_t kMaxOutput = 262144;

bool write_all(int fd, const std::string& body) {
    size_t written = 0;
    while (written < body.size()) {
        const ssize_t count = write(fd, body.data() + written, body.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

void log_line(const std::string& message) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
    std::fprintf(stdout, "%s %s\n", stamp, message.c_str());
    std::fflush(stdout);
}

bool read_file(const std::string& path, std::string& out) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    out.clear();
    char buffer[4096];
    while (true) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            out.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& body) {
    const std::string temp = path + ".tmp";
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    if (!write_all(fd, body) || fsync(fd) != 0) {
        close(fd);
        unlink(temp.c_str());
        return false;
    }
    close(fd);
    if (rename(temp.c_str(), path.c_str()) != 0) {
        unlink(temp.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
}

std::string trim(std::string text) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    size_t begin = 0;
    while (begin < text.size() && is_space(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

RunResult run(const std::vector<std::string>& argv, int timeout_seconds) {
    int fds[2];
    if (pipe(fds) != 0) {
        return {-1, std::string("pipe: ") + std::strerror(errno)};
    }

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& argument : argv) {
        args.push_back(const_cast<char*>(argument.c_str()));
    }
    args.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return {-1, std::string("fork: ") + std::strerror(errno)};
    }
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(args[0], args.data());
        _exit(127);
    }

    close(fds[1]);
    RunResult result;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool timed_out = false;
    char buffer[1024];
    while (true) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining <= 0) {
            timed_out = true;
            break;
        }
        pollfd descriptor{fds[0], POLLIN, 0};
        const int ready =
            poll(&descriptor, 1, static_cast<int>(std::min<long long>(remaining, 500)));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        const ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            if (result.output.size() < kMaxOutput) {
                result.output.append(buffer, static_cast<size_t>(count));
            }
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    close(fds[0]);
    if (timed_out) {
        kill(pid, SIGKILL);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out) {
        result.status = 124;
    } else if (WIFEXITED(status)) {
        result.status = WEXITSTATUS(status);
    } else {
        result.status = 1;
    }
    result.output = trim(std::move(result.output));
    return result;
}

bool succeeded(const RunResult& result) { return result.status == 0; }

std::string describe(const RunResult& result) {
    if (!result.output.empty()) {
        return result.output;
    }
    return "exit status " + std::to_string(result.status);
}

long long host_cpu_count() {
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? count : 1;
}

long long host_ram_bytes() {
    std::string text;
    long long kilobytes = 0;
    if (read_file("/proc/meminfo", text) &&
        std::sscanf(text.c_str(), "MemTotal: %lld kB", &kilobytes) == 1 &&
        kilobytes > 0) {
        return kilobytes * 1024;
    }
    return 8LL * 1024 * 1024 * 1024;
}

}  // namespace sys
