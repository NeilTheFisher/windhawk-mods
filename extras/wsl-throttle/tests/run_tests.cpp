// Unit tests for the parts of the agent where a silent bug would strand a
// container or quietly stop enforcing the policy: JSON parsing, policy
// interpretation, and the docker command/limit model.
//
// No framework: `make test` builds and runs this binary.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "docker.hpp"
#include "json.hpp"
#include "policy.hpp"
#include "sys.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("  FAIL line %d: %s\n", line, expression);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void section(const char* name) { std::printf("- %s\n", name); }

std::string join(const std::vector<std::string>& parts) {
    std::string merged;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            merged += " ";
        }
        merged += parts[i];
    }
    return merged;
}

// Exactly what the mod writes, including the field the mod added last.
const char* kThrottleState =
    R"({"version":1,"mode":"throttle","reason":"wsl-background","generated":1794204671,)"
    R"("cpu_quota_percent":35,"docker":{"enabled":true,"cpus_centi":150,)"
    R"("memory_mb":0},"drop_caches":false,"distro":""})";

const char* kRestoreState =
    R"({"version":1,"mode":"restore","reason":"gaming","generated":1794204671,)"
    R"("cpu_quota_percent":35,"docker":{"enabled":false,"cpus_centi":100,)"
    R"("memory_mb":512},"drop_caches":true,"distro":"Ubuntu"})";

void test_json() {
    section("json: parses the state the mod writes");
    json::Value root;
    std::string error;
    CHECK(json::parse(kThrottleState, root, error));
    CHECK(root.is_object());
    CHECK(json::as_string(root.get("mode")) == "throttle");
    CHECK(json::as_int(root.get("generated"), 0, 0, 1LL << 40) == 1794204671LL);
    const json::Value* docker = root.get("docker");
    CHECK(docker != nullptr && docker->is_object());
    CHECK(json::as_int(docker->get("cpus_centi"), 0, 0, 6400) == 150);
    CHECK(json::as_bool(root.get("drop_caches"), true) == false);

    section("json: rejects malformed input instead of guessing");
    const char* bad[] = {"",    "{",      "{\"a\":}", "nul",
                         "{\"a\"", "[1,2,", "{} x",    "{\"a\":1,}"};
    for (const char* text : bad) {
        json::Value value;
        std::string reason;
        CHECK(!json::parse(text, value, reason));
        CHECK(!reason.empty());
    }

    section("json: round trips strings with escapes");
    json::Value built = json::object_value();
    json::set(built, "quoted", json::string_value("a \"b\" \\ c\n\t"));
    json::Value parsed;
    std::string reason;
    CHECK(json::parse(json::serialize(built), parsed, reason));
    CHECK(json::as_string(parsed.get("quoted")) == "a \"b\" \\ c\n\t");

    section("json: numbers clamp, wrong types fall back");
    json::Value numbers = json::object_value();
    json::set(numbers, "low", json::number_value(1));
    json::set(numbers, "high", json::number_value(999));
    json::set(numbers, "text", json::string_value("42"));
    CHECK(json::as_int(numbers.get("low"), 35, 5, 90) == 5);
    CHECK(json::as_int(numbers.get("high"), 35, 5, 90) == 90);
    CHECK(json::as_int(numbers.get("text"), 35, 5, 90) == 35);
    CHECK(json::as_int(numbers.get("missing"), 7, 0, 90) == 7);
    CHECK(json::as_int(nullptr, 7, 0, 90) == 7);
}

void test_policy() {
    section("policy: throttle and restore parse");
    policy::Policy value;
    std::string error;
    CHECK(policy::parse(kThrottleState, value, error));
    CHECK(value.throttle);
    CHECK(value.reason == "wsl-background");
    CHECK(value.cpu_percent == 35);
    CHECK(value.docker_enabled);
    CHECK(value.cpus_centi == 150);
    CHECK(value.memory_mb == 0);
    CHECK(!value.drop_caches);
    CHECK(value.distro.empty());

    CHECK(policy::parse(kRestoreState, value, error));
    CHECK(!value.throttle);
    CHECK(value.reason == "gaming");
    CHECK(!value.docker_enabled);
    CHECK(value.cpus_centi == 100);
    CHECK(value.memory_mb == 512);
    CHECK(value.drop_caches);
    CHECK(value.distro == "Ubuntu");

    section("policy: an unknown mode is an error, not a guess");
    value = policy::Policy{};
    error.clear();
    CHECK(!policy::parse(R"({"mode":"pause"})", value, error));
    CHECK(error.find("mode") != std::string::npos);

    section("policy: missing docker block falls back to safe defaults");
    CHECK(policy::parse(R"({"mode":"throttle"})", value, error));
    CHECK(value.throttle);
    CHECK(value.docker_enabled);  // absent means enabled
    CHECK(value.cpus_centi == 150);
    CHECK(value.memory_mb == 0);
    CHECK(value.cpu_percent == 35);

    section("policy: out-of-range values clamp instead of being trusted");
    CHECK(policy::parse(
        R"({"mode":"throttle","cpu_quota_percent":1,"docker":{"cpus_centi":99999}})",
        value, error));
    CHECK(value.cpu_percent == 5);
    CHECK(value.cpus_centi == 6400);

    section("policy: distro gate");
    CHECK(policy::parse(kThrottleState, value, error));
    policy::gate_on_distro(value, "Ubuntu");
    CHECK(value.throttle);  // empty policy distro accepts any distro
    value.distro = "Ubuntu";
    policy::gate_on_distro(value, "Ubuntu");
    CHECK(value.throttle);
    value.distro = "ubuntu";
    policy::gate_on_distro(value, "Ubuntu");
    CHECK(value.throttle);  // distro names are typed by hand: case-insensitive
    value.distro = "Debian";
    policy::gate_on_distro(value, "Ubuntu");
    CHECK(!value.throttle);
    CHECK(value.reason == "distro-mismatch");
    value.distro = "Debian";
    value.throttle = true;
    policy::gate_on_distro(value, "");
    CHECK(value.throttle);  // agent cannot prove otherwise, so it accepts

    section("policy: staleness");
    CHECK(policy::parse(kThrottleState, value, error));
    CHECK(!policy::is_stale(value, 1794204671 + 10, 300));
    CHECK(policy::is_stale(value, 1794204671 + 301, 300));
    value.throttle = false;
    CHECK(!policy::is_stale(value, 1794204671 + 9999, 300));

    section("policy: applied state folds mode into the signature");
    CHECK(policy::parse(kThrottleState, value, error));
    policy::Applied applied = policy::applied_from(value);
    CHECK(applied.throttle);
    CHECK(applied.cpu_percent == 35);
    CHECK(applied.docker_enabled);
    value.throttle = false;
    applied = policy::applied_from(value);
    CHECK(!applied.throttle);
    CHECK(applied.cpu_percent == 0);
    CHECK(!applied.docker_enabled);

    section("policy: token sanitizer mirrors the mod");
    CHECK(policy::sanitize_token("Ubuntu-22.04") == "Ubuntu-22.04");
    CHECK(policy::sanitize_token("Ubuntu (WSL)") == "Ubuntu (WSL)");
    CHECK(policy::sanitize_token("bad\"name\\n\n") == "badnamen");
    CHECK(policy::sanitize_token(std::string(100, 'a')).size() == 64);
}

void test_docker() {
    section("docker: update argv without a memory limit");
    docker::Limits limits;
    limits.nano = 1500000000;
    limits.shares = 256;
    CHECK(join(docker::update_argv(limits, {"abc", "def"})) ==
          "docker update --cpus 1.50 --cpu-shares 256 abc def");

    section("docker: update argv with a memory limit and default swap");
    limits.mem = 64LL * 1024 * 1024;
    CHECK(join(docker::update_argv(limits, {"abc"})) ==
          "docker update --cpus 1.50 --cpu-shares 256 --memory 67108864 "
          "--memory-swap 134217728 abc");

    section("docker: explicit swap is preserved on restore");
    limits.mem = 100663296;
    limits.swap = 100663296;  // container's original swap, not 2x memory
    CHECK(join(docker::update_argv(limits, {"abc"})) ==
          "docker update --cpus 1.50 --cpu-shares 256 --memory 100663296 "
          "--memory-swap 100663296 abc");

    section("docker: whole-core limits keep two decimals");
    limits = docker::Limits{};
    limits.nano = 16000000000LL;
    limits.shares = 1024;
    CHECK(join(docker::update_argv(limits, {})) ==
          "docker update --cpus 16.00 --cpu-shares 1024");

    section("docker: identical originals share one group, different ones do not");
    docker::Limits one;
    one.nano = 16000000000LL;
    one.shares = 1024;
    docker::Limits two = one;
    docker::Limits three = one;
    three.nano = 2000000000LL;
    std::map<docker::Limits, std::vector<std::string>> groups;
    groups[one].push_back("a");
    groups[two].push_back("b");
    groups[three].push_back("c");
    CHECK(groups.size() == 2);
    CHECK(groups[one].size() == 2);
}

void test_sys() {
    section("sys: split keeps empty fields but trims around them");
    const auto lines = sys::split("a\n\nb\n", '\n');
    CHECK(lines.size() == 4);
    CHECK(lines[0] == "a");
    CHECK(lines[1].empty());
    CHECK(lines[2] == "b");
    const auto fields = sys::split("0|0|67108864|0", '|');
    CHECK(fields.size() == 4);
    CHECK(fields[2] == "67108864");

    section("sys: trim");
    CHECK(sys::trim("  x \r\n") == "x");
    CHECK(sys::trim("").empty());
    CHECK(sys::trim("   ").empty());

    section("sys: host facts are sane");
    CHECK(sys::host_cpu_count() >= 1);
    CHECK(sys::host_ram_bytes() > 0);
}

}  // namespace

int main() {
    test_json();
    test_policy();
    test_docker();
    test_sys();

    if (g_failures == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
