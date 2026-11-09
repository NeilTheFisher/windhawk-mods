#pragma once

#include <string>
#include <string_view>
#include <vector>

// Thin OS primitives: files, subprocesses, logging, host facts. Nothing here
// knows about WSL, docker or the throttle policy.
namespace sys {

void log_line(const std::string& message);

bool read_file(const std::string& path, std::string& out);
// Write + rename, so a reader never observes a half-written file.
bool write_file_atomic(const std::string& path, const std::string& body);

std::vector<std::string> split(std::string_view text, char separator);
std::string trim(std::string text);

struct RunResult {
    int status = -1;
    std::string output;  // stdout and stderr merged
};

RunResult run(const std::vector<std::string>& argv, int timeout_seconds);
bool succeeded(const RunResult& result);
std::string describe(const RunResult& result);

long long host_cpu_count();
long long host_ram_bytes();

}  // namespace sys
