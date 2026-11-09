#pragma once

#include <string>
#include <string_view>
#include <tuple>

// The published policy, and the pure rules that interpret it. Everything here
// is free of I/O so it can be tested directly.
namespace policy {

struct Policy {
    bool throttle = false;
    std::string reason = "startup";
    long long generated = 0;
    int cpu_percent = 35;
    bool docker_enabled = true;
    int cpus_centi = 150;
    int memory_mb = 0;
    bool drop_caches = false;
    std::string distro;
};

// What was last successfully applied. A struct rather than a magic string, so
// "did the policy change" is a plain comparison.
struct Applied {
    bool throttle = false;
    int cpu_percent = 0;
    bool docker_enabled = false;
    int cpus_centi = 0;
    int memory_mb = 0;

    bool operator==(const Applied& other) const;
};

// Mirror of the mod's SanitizeWslToken: the two sides must agree exactly.
std::string sanitize_token(std::string_view value);

// Parses state.json. Returns false with `error` set on anything malformed.
bool parse(const std::string& body, Policy& out, std::string& error);

// A policy addressed to another distro must never be applied here. Comparison
// is case-insensitive because distro names are typed by hand in the settings.
void gate_on_distro(Policy& policy, const std::string& own_distro);

bool same_distro(std::string_view left, std::string_view right);

// True when the mod has stopped refreshing state.json, which means restoring.
bool is_stale(const Policy& policy, long long now, long long max_age);

Applied applied_from(const Policy& policy);

}  // namespace policy
