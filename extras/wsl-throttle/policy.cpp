#include "policy.hpp"

#include <cctype>

#include "json.hpp"

namespace policy {
namespace {

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

bool Applied::operator==(const Applied& other) const {
    return std::tie(throttle, cpu_percent, docker_enabled, cpus_centi,
                    memory_mb) ==
           std::tie(other.throttle, other.cpu_percent, other.docker_enabled,
                    other.cpus_centi, other.memory_mb);
}

std::string sanitize_token(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char c : value) {
        const bool keep = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
                          c == '_' || c == ' ' || c == '(' || c == ')';
        if (keep && result.size() < 64) {
            result.push_back(c);
        }
    }
    return result;
}

bool parse(const std::string& body, Policy& out, std::string& error) {
    json::Value root;
    if (!json::parse(body, root, error)) {
        error = "state.json is not valid JSON: " + error;
        return false;
    }
    if (!root.is_object()) {
        error = "state.json is not an object";
        return false;
    }
    const std::string mode = json::as_string(root.get("mode"));
    if (mode != "throttle" && mode != "restore") {
        error = "state.json has unknown mode '" + mode + "'";
        return false;
    }

    const json::Value* docker = root.get("docker");
    Policy policy;
    policy.throttle = mode == "throttle";
    policy.reason = json::as_string(root.get("reason")).substr(0, 64);
    policy.generated = json::as_int(root.get("generated"), 0, 0, 1LL << 40);
    policy.cpu_percent =
        static_cast<int>(json::as_int(root.get("cpu_quota_percent"), 35, 5, 90));
    policy.docker_enabled =
        json::as_bool(docker != nullptr ? docker->get("enabled") : nullptr, true);
    policy.cpus_centi = static_cast<int>(json::as_int(
        docker != nullptr ? docker->get("cpus_centi") : nullptr, 150, 10, 6400));
    policy.memory_mb = static_cast<int>(json::as_int(
        docker != nullptr ? docker->get("memory_mb") : nullptr, 0, 0, 262144));
    policy.drop_caches = json::as_bool(root.get("drop_caches"), false);
    policy.distro = sanitize_token(json::as_string(root.get("distro")));

    out = std::move(policy);
    return true;
}

bool same_distro(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (lower(left[i]) != lower(right[i])) {
            return false;
        }
    }
    return true;
}

void gate_on_distro(Policy& policy, const std::string& own_distro) {
    if (policy.distro.empty() || own_distro.empty()) {
        return;  // unanswered question: accept the policy
    }
    if (!same_distro(policy.distro, own_distro)) {
        policy.throttle = false;
        policy.reason = "distro-mismatch";
    }
}

bool is_stale(const Policy& policy, long long now, long long max_age) {
    return policy.throttle && max_age > 0 && policy.generated > 0 &&
           now - policy.generated > max_age;
}

Applied applied_from(const Policy& policy) {
    Applied applied;
    applied.throttle = policy.throttle;
    applied.cpu_percent = policy.throttle ? policy.cpu_percent : 0;
    applied.docker_enabled = policy.throttle && policy.docker_enabled;
    applied.cpus_centi = policy.cpus_centi;
    applied.memory_mb = policy.memory_mb;
    return applied;
}

}  // namespace policy
