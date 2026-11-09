#include "docker.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <tuple>

#include "sys.hpp"

namespace docker {
namespace {

constexpr int kCommandTimeoutSeconds = 40;
constexpr int kDefaultShares = 1024;

std::string format_cpus(long long nano) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(nano) / 1e9);
    return buffer;
}


bool parse_number(const std::string& field, long long& out) {
    const std::string text = sys::trim(field);
    if (text.empty()) {
        out = 0;
        return true;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value < 0) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

std::vector<std::string> update_argv(const Limits& limits,
                                     const std::vector<std::string>& ids) {
    std::vector<std::string> argv{"docker",
                                  "update",
                                  "--cpus",
                                  format_cpus(limits.nano),
                                  "--cpu-shares",
                                  std::to_string(limits.shares)};
    if (limits.mem > 0) {
        argv.push_back("--memory");
        argv.push_back(std::to_string(limits.mem));
        argv.push_back("--memory-swap");
        argv.push_back(std::to_string(limits.swap > 0 ? limits.swap
                                                      : limits.mem * 2));
    }
    argv.insert(argv.end(), ids.begin(), ids.end());
    return argv;
}

bool Limits::operator<(const Limits& other) const {
    return std::tie(nano, shares, mem, swap) <
           std::tie(other.nano, other.shares, other.mem, other.swap);
}

bool running_ids(std::vector<std::string>& ids, std::string& error) {
    const sys::RunResult result = sys::run({"docker", "ps", "-q"}, 20);
    if (!sys::succeeded(result)) {
        error = sys::describe(result);
        return false;
    }
    ids.clear();
    for (const auto& line : sys::split(result.output, '\n')) {
        const std::string id = sys::trim(line);
        if (!id.empty()) {
            ids.push_back(id);
        }
    }
    return true;
}

bool originals_of(const std::vector<std::string>& ids, bool memory_limited,
                  std::map<std::string, Original>& out, std::string& error) {
    if (ids.empty()) {
        return true;
    }
    std::vector<std::string> argv{
        "docker", "inspect", "-f",
        "{{.HostConfig.NanoCpus}}|{{.HostConfig.CpuShares}}"
        "|{{.HostConfig.Memory}}|{{.HostConfig.MemorySwap}}"};
    argv.insert(argv.end(), ids.begin(), ids.end());

    const sys::RunResult result = sys::run(argv, kCommandTimeoutSeconds);
    if (!sys::succeeded(result)) {
        error = sys::describe(result);
        return false;
    }

    // docker inspect prints one row per container, in argument order.
    const auto rows = sys::split(result.output, '\n');
    size_t row = 0;
    for (const auto& id : ids) {
        while (row < rows.size() && sys::trim(rows[row]).empty()) {
            ++row;
        }
        if (row >= rows.size()) {
            error = "docker inspect returned fewer rows than containers";
            return false;
        }
        const auto fields = sys::split(rows[row++], '|');
        if (fields.size() != 4) {
            error = "docker inspect returned an unexpected row";
            return false;
        }
        Original original;
        if (!parse_number(fields[0], original.nano) ||
            !parse_number(fields[1], original.shares) ||
            !parse_number(fields[2], original.mem) ||
            !parse_number(fields[3], original.swap)) {
            error = "docker inspect returned a non-numeric limit";
            return false;
        }
        original.mem_applied = memory_limited;
        out[id] = original;
    }
    return true;
}

int apply(const Limits& limits, const std::vector<std::string>& ids,
          std::vector<std::string>& errors) {
    if (ids.empty()) {
        return 0;
    }
    const sys::RunResult result =
        sys::run(update_argv(limits, ids), kCommandTimeoutSeconds);
    if (!sys::succeeded(result)) {
        errors.push_back("docker update: " + sys::describe(result));
        return 0;
    }
    return static_cast<int>(ids.size());
}

std::vector<std::string> release(const std::vector<std::string>& live,
                                 const std::map<std::string, Original>& originals,
                                 std::vector<std::string>& errors) {
    std::map<Limits, std::vector<std::string>> groups;
    for (const auto& id : live) {
        const auto entry = originals.find(id);
        if (entry == originals.end()) {
            continue;
        }
        const Original& original = entry->second;
        Limits limits;
        limits.nano = original.nano > 0 ? original.nano : sys::host_cpu_count() * 1000000000LL;
        limits.shares = original.shares > 0 ? original.shares : kDefaultShares;
        if (original.mem_applied) {
            limits.mem = original.mem > 0 ? original.mem : sys::host_ram_bytes();
            limits.swap = original.swap;
        }
        groups[limits].push_back(id);
    }

    std::vector<std::string> restored;
    for (const auto& group : groups) {
        const sys::RunResult result =
            sys::run(update_argv(group.first, group.second), kCommandTimeoutSeconds);
        if (!sys::succeeded(result)) {
            errors.push_back("docker update (restore): " + sys::describe(result));
            continue;
        }
        restored.insert(restored.end(), group.second.begin(), group.second.end());
    }
    return restored;
}

}  // namespace docker
