// wsl-throttle-agent: applies the background-throttle policy that the Windhawk
// "Smart Process Priority & RAM Optimizer" mod publishes from the Windows side.
//
// The mod writes state.json into %LOCALAPPDATA%\WslThrottle; this agent
// reconciles that policy and writes status.json back so the mod can log what
// happened. Nothing in the state file is ever interpreted as a command: every
// field is range-checked and subprocesses are only ever spawned from fixed argv
// vectors.
//
// Cost model, because that is the whole point of this binary:
//   * steady state is one stat() per second - no subprocess, no docker call.
//     Reconciles are signature-driven, so an unchanged policy costs nothing.
//   * docker is enumerated only when the policy changes or on a slow rescan,
//     which is how a new container still gets picked up within seconds.
//   * identical limits are applied in a single `docker update`, since one call
//     takes any number of containers.
//   * the cgroup quota goes through systemd rather than writing cpu.max
//     directly: systemd owns user.slice, and a direct write would be reverted
//     the next time it reconciles the unit.

#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "docker.hpp"
#include "json.hpp"
#include "policy.hpp"
#include "sys.hpp"

namespace {

constexpr const char* kRunDir = "/run/wsl-throttle";
constexpr const char* kOriginalsPath = "/run/wsl-throttle/originals.json";
constexpr const char* kLockPath = "/run/wsl-throttle/lock";
// Written by install.sh from $WSL_DISTRO_NAME. When present, a policy addressed
// to another distro is ignored instead of applied to the wrong one.
constexpr const char* kDistroFile = "/etc/wsl-throttle/distro";
constexpr const char* kStateGlob = "/mnt/c/Users/*/AppData/Local/WslThrottle";

constexpr int kPollSeconds = 1;
constexpr int kRescanSeconds = 15;
// If the mod stops refreshing state.json (unloaded, crashed, machine moved) the
// agent restores instead of stranding the distro.
constexpr long long kStaleSeconds = 300;
constexpr int kQuotaTimeoutSeconds = 25;
constexpr int kThrottledCpuShares = 256;

// A distro that is doing real work must never be capped. The mod can only see
// Windows-side windows, so a workload driven through a GUI (an editor, an agent
// shell, a TUI) looks idle from there and gets capped mid-build. This is the
// ground truth, measured from the cgroup itself.
constexpr const char* kUserSliceCpuStat = "/sys/fs/cgroup/user.slice/cpu.stat";
constexpr double kBusyCores = 0.20;   // one fifth of a core
constexpr int kBusyEnterSamples = 2;
constexpr int kBusyLeaveSamples = 20;  // ~20s of quiet before we may cap again

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

const std::string& own_distro() {
    static const std::string name = [] {
        std::string text;
        if (!sys::read_file(kDistroFile, text)) {
            const char* from_env = std::getenv("WSL_DISTRO_NAME");
            text = from_env != nullptr ? from_env : "";
        }
        return policy::sanitize_token(sys::trim(std::move(text)));
    }();
    return name;
}

class CpuActivity {
  public:
    // Samples once per call; call it exactly once per poll tick.
    bool busy() {
        std::string text;
        long long usage = 0;
        if (!sys::read_file(kUserSliceCpuStat, text) || !parse_usage(text, usage)) {
            return busy_;  // unreadable: keep the last verdict rather than flap
        }
        const auto now = std::chrono::steady_clock::now();
        if (last_usage_) {
            const double seconds =
                std::chrono::duration<double>(now - last_time_).count();
            if (seconds > 0.2) {
                const double cores =
                    static_cast<double>(usage - *last_usage_) / 1e6 / seconds;
                if (cores >= kBusyCores) {
                    ++hot_samples_;
                    quiet_samples_ = 0;
                } else {
                    ++quiet_samples_;
                    hot_samples_ = 0;
                }
                if (!busy_ && hot_samples_ >= kBusyEnterSamples) {
                    busy_ = true;
                } else if (busy_ && quiet_samples_ >= kBusyLeaveSamples) {
                    busy_ = false;
                }
            }
        }
        last_usage_ = usage;
        last_time_ = now;
        return busy_;
    }

  private:
    static bool parse_usage(const std::string& text, long long& out) {
        const size_t position = text.find("usage_usec");
        if (position == std::string::npos) {
            return false;
        }
        char* end = nullptr;
        const long long value =
            std::strtoll(text.c_str() + position + 10, &end, 10);
        if (end == text.c_str() + position + 10 || value < 0) {
            return false;
        }
        out = value;
        return true;
    }

    std::optional<long long> last_usage_;
    std::chrono::steady_clock::time_point last_time_{};
    int hot_samples_ = 0;
    int quiet_samples_ = 0;
    bool busy_ = false;
};

struct Report {
    std::string mode;
    std::string reason;
    int cpu_percent = 0;
    int containers = 0;
    std::string distro;
    bool busy = false;
    std::vector<std::string> errors;

    bool operator==(const Report& other) const {
        return mode == other.mode && reason == other.reason &&
               cpu_percent == other.cpu_percent &&
               containers == other.containers && distro == other.distro &&
               busy == other.busy && errors == other.errors;
    }
};

json::Value report_json(const Report& report) {
    json::Value root = json::object_value();
    json::set(root, "mode", json::string_value(report.mode));
    json::set(root, "reason", json::string_value(report.reason));
    json::set(root, "cpu_quota_percent", json::number_value(report.cpu_percent));
    json::set(root, "containers", json::number_value(report.containers));
    json::set(root, "distro", json::string_value(report.distro));
    json::set(root, "busy", json::bool_value(report.busy));
    json::Value errors = json::array_value();
    for (const auto& problem : report.errors) {
        json::push(errors, json::string_value(problem));
    }
    json::set(root, "errors", std::move(errors));
    return root;
}

std::optional<std::string> find_state_dir() {
    glob_t matches{};
    std::optional<std::string> found;
    if (glob(kStateGlob, 0, nullptr, &matches) == 0) {
        std::vector<std::string> candidates(matches.gl_pathv,
                                            matches.gl_pathv + matches.gl_pathc);
        std::sort(candidates.begin(), candidates.end());
        for (const auto& candidate : candidates) {
            if (access((candidate + "/state.json").c_str(), R_OK) == 0) {
                found = candidate;
                break;
            }
        }
    }
    globfree(&matches);
    return found;
}

class Agent {
  public:
    Agent() { load_originals(); }

    std::optional<Report> step();
    void run_reset();
    void print_status() const;

  private:
    struct FileStamp {
        long long seconds = 0;
        long long nanoseconds = 0;

        bool operator==(const FileStamp& other) const {
            return seconds == other.seconds && nanoseconds == other.nanoseconds;
        }
    };

    bool directory_ready();
    bool refresh_policy(std::string& error);
    Report reconcile(const policy::Policy& value);
    void reconcile_containers(const policy::Policy& value,
                              std::vector<std::string>& errors);
    void forget_dead(const std::vector<std::string>& live);
    void write_status(const Report& report) const;
    void write_status_error(const std::string& error) const;
    void load_originals();
    void save_originals() const;

    std::optional<std::string> directory_;
    std::map<std::string, docker::Original> originals_;
    std::optional<policy::Applied> applied_;
    std::optional<Report> last_report_;
    std::optional<policy::Policy> policy_;
    std::optional<FileStamp> policy_stamp_;
    std::optional<std::chrono::steady_clock::time_point> last_scan_;
    CpuActivity activity_;
};

bool Agent::directory_ready() {
    if (!directory_) {
        directory_ = find_state_dir();
    }
    return directory_.has_value();
}

bool Agent::refresh_policy(std::string& error) {
    const std::string path = *directory_ + "/state.json";
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        error = std::string("cannot stat state.json: ") + std::strerror(errno);
        return false;
    }
    const FileStamp stamp{static_cast<long long>(info.st_mtim.tv_sec),
                          static_cast<long long>(info.st_mtim.tv_nsec)};
    if (policy_ && policy_stamp_ && *policy_stamp_ == stamp) {
        return true;  // fast path: one stat, no read, no parse
    }
    policy_stamp_ = stamp;

    std::string body;
    if (!sys::read_file(path, body)) {
        error = std::string("cannot read state.json: ") + std::strerror(errno);
        return false;
    }
    policy::Policy parsed;
    if (!policy::parse(body, parsed, error)) {
        return false;
    }
    policy::gate_on_distro(parsed, own_distro());
    policy_ = std::move(parsed);
    return true;
}

Report Agent::reconcile(const policy::Policy& value) {
    Report report;
    report.mode = value.throttle ? "throttle" : "restore";
    report.reason = value.reason;
    report.cpu_percent = value.throttle ? value.cpu_percent : 0;
    report.distro = own_distro();

    std::vector<std::string> errors;
    const policy::Applied target = policy::applied_from(value);
    const bool changed = !applied_ || !(*applied_ == target);

    if (changed) {
        const std::string quota =
            value.throttle
                ? "CPUQuota=" + std::to_string(value.cpu_percent) + "%"
                : std::string("CPUQuota=");
        const sys::RunResult result = sys::run(
            {"systemctl", "set-property", "--runtime", "user.slice", quota},
            kQuotaTimeoutSeconds);
        if (!sys::succeeded(result)) {
            errors.push_back("user.slice " + quota + " failed: " +
                             sys::describe(result));
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const bool rescan_due = !last_scan_ ||
                            now - *last_scan_ >= std::chrono::seconds(kRescanSeconds);
    if (changed || rescan_due) {
        reconcile_containers(value, errors);
        last_scan_ = now;
    }
    // "containers" is what is currently under policy, not what this tick
    // happened to touch, otherwise the reported count flaps to 0 between
    // rescans and every flare rewrites status.json.
    report.containers = static_cast<int>(originals_.size());

    if (value.throttle && changed && value.drop_caches) {
        sys::run({"sync"}, 30);
        const int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            errors.push_back(std::string("drop_caches: ") + std::strerror(errno));
        } else {
            if (write(fd, "3", 1) != 1) {
                errors.push_back(std::string("drop_caches: ") +
                                 std::strerror(errno));
            }
            close(fd);
        }
    }

    report.errors = errors;
    applied_ = errors.empty() ? std::optional<policy::Applied>(target)
                              : std::nullopt;
    return report;
}

void Agent::reconcile_containers(const policy::Policy& value,
                                 std::vector<std::string>& errors) {
    std::vector<std::string> live;
    std::string error;
    if (!docker::running_ids(live, error)) {
        if (value.docker_enabled) {
            errors.push_back("docker ps: " + error);
        }
        return;
    }
    forget_dead(live);

    if (!value.throttle || !value.docker_enabled) {
        // Turning the docker half off has to release what it capped, not leave
        // containers limited until the next mode change.
        const auto restored = docker::release(live, originals_, errors);
        for (const auto& id : restored) {
            originals_.erase(id);
        }
        if (!restored.empty()) {
            sys::log_line("restored limits on " + std::to_string(restored.size()) +
                          " container(s)");
            save_originals();
        }
        return;
    }

    std::vector<std::string> untracked;
    for (const auto& id : live) {
        if (originals_.find(id) == originals_.end()) {
            untracked.push_back(id);
        }
    }
    if (!untracked.empty()) {
        if (!docker::originals_of(untracked, value.memory_mb > 0, originals_,
                                  error)) {
            errors.push_back("docker inspect: " + error);
        }
        save_originals();
    }

    docker::Limits limits;
    limits.nano = static_cast<long long>(value.cpus_centi) * 10000000LL;
    limits.shares = kThrottledCpuShares;
    limits.mem = static_cast<long long>(value.memory_mb) * 1024 * 1024;
    docker::apply(limits, live, errors);
}

void Agent::forget_dead(const std::vector<std::string>& live) {
    for (auto it = originals_.begin(); it != originals_.end();) {
        if (std::find(live.begin(), live.end(), it->first) == live.end()) {
            it = originals_.erase(it);
            continue;
        }
        ++it;
    }
}

void Agent::load_originals() {
    std::string body;
    if (!sys::read_file(kOriginalsPath, body)) {
        return;
    }
    json::Value root;
    std::string error;
    if (!json::parse(body, root, error) || !root.is_object()) {
        return;
    }
    for (const auto& entry : root.object) {
        if (!entry.second.is_object()) {
            continue;
        }
        docker::Original original;
        original.nano = json::as_int(entry.second.get("nano"), 0, 0, 1LL << 40);
        original.shares = json::as_int(entry.second.get("shares"), 0, 0, 1LL << 40);
        original.mem = json::as_int(entry.second.get("mem"), 0, 0, 1LL << 40);
        original.swap = json::as_int(entry.second.get("swap"), 0, 0, 1LL << 40);
        original.mem_applied =
            json::as_bool(entry.second.get("mem_applied"), false);
        originals_[entry.first] = original;
    }
}

void Agent::save_originals() const {
    mkdir(kRunDir, 0755);
    json::Value root = json::object_value();
    for (const auto& entry : originals_) {
        json::Value node = json::object_value();
        json::set(node, "nano", json::number_value(static_cast<double>(entry.second.nano)));
        json::set(node, "shares", json::number_value(static_cast<double>(entry.second.shares)));
        json::set(node, "mem", json::number_value(static_cast<double>(entry.second.mem)));
        json::set(node, "swap", json::number_value(static_cast<double>(entry.second.swap)));
        json::set(node, "mem_applied", json::bool_value(entry.second.mem_applied));
        json::set(root, entry.first, std::move(node));
    }
    sys::write_file_atomic(kOriginalsPath, json::serialize(root));
}

void Agent::write_status(const Report& report) const {
    json::Value root = report_json(report);
    json::set(root, "agent", json::string_value("ok"));
    json::set(root, "updated", json::number_value(static_cast<double>(std::time(nullptr))));
    sys::write_file_atomic(*directory_ + "/status.json", json::serialize(root));
}

void Agent::write_status_error(const std::string& error) const {
    json::Value root = json::object_value();
    json::set(root, "agent", json::string_value("error"));
    json::set(root, "error", json::string_value(error));
    json::set(root, "updated", json::number_value(static_cast<double>(std::time(nullptr))));
    sys::write_file_atomic(*directory_ + "/status.json", json::serialize(root));
}

std::optional<Report> Agent::step() {
    if (!directory_ready()) {
        return std::nullopt;
    }
    std::string error;
    if (!refresh_policy(error)) {
        write_status_error(error);
        return std::nullopt;
    }

    const bool busy = activity_.busy();
    policy::Policy value = *policy_;
    if (policy::is_stale(value, std::time(nullptr), kStaleSeconds)) {
        value.throttle = false;
        value.reason = "state-stale";
    } else if (value.throttle && busy) {
        // Someone is working in the distro right now: releasing beats a cap
        // that makes their build crawl.
        value.throttle = false;
        value.reason = "wsl-busy";
    }

    Report report = reconcile(value);
    report.busy = busy;
    if (!last_report_ || !(*last_report_ == report)) {
        last_report_ = report;
        write_status(report);
    }
    return report;
}

void Agent::run_reset() {
    if (directory_ready()) {
        Report report;
        report.mode = "restore";
        report.reason = "manual-reset";
        report.distro = own_distro();
        write_status(report);
    }
    applied_.reset();
    policy::Policy value;
    value.throttle = false;
    value.reason = "manual-reset";
    const Report report = reconcile(value);
    sys::log_line("mode=" + report.mode + " reason=" + report.reason +
                  " containers=" + std::to_string(report.containers));
}

void Agent::print_status() const {
    json::Value root = json::object_value();
    json::set(root, "state_dir",
              json::string_value(directory_ ? *directory_ : std::string()));
    json::set(root, "tracked_containers",
              json::number_value(static_cast<double>(originals_.size())));
    if (applied_) {
        json::Value applied = json::object_value();
        json::set(applied, "mode",
                  json::string_value(applied_->throttle ? "throttle" : "restore"));
        json::set(applied, "cpu_percent",
                  json::number_value(applied_->cpu_percent));
        json::set(applied, "docker",
                  json::bool_value(applied_->docker_enabled));
        json::set(applied, "cpus_centi",
                  json::number_value(applied_->cpus_centi));
        json::set(applied, "memory_mb",
                  json::number_value(applied_->memory_mb));
        json::set(root, "applied", std::move(applied));
    }
    std::printf("%s\n", json::serialize(root).c_str());
}

bool acquire_lock() {
    mkdir(kRunDir, 0755);
    const int fd = open(kLockPath, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    return true;  // the lock lives as long as the process does
}

void install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

std::string describe_errors(const Report& report) {
    std::string message;
    for (size_t i = 0; i < report.errors.size(); ++i) {
        message += i == 0 ? " errors=" : "; ";
        message += report.errors[i];
    }
    return message;
}

int run_daemon(Agent& agent) {
    if (!acquire_lock()) {
        sys::log_line("another agent instance is running");
        return 0;
    }
    install_signal_handlers();
    sys::log_line("started (poll " + std::to_string(kPollSeconds) +
                  "s, stale-after " + std::to_string(kStaleSeconds) + "s)");

    std::optional<Report> last_logged;
    while (!g_stop) {
        if (const auto report = agent.step()) {
            if (!last_logged || !(*last_logged == *report)) {
                last_logged = report;
                sys::log_line("mode=" + report->mode + " reason=" +
                              report->reason + " containers=" +
                              std::to_string(report->containers) +
                              describe_errors(*report));
            }
        }
        sleep(kPollSeconds);
    }
    sys::log_line("stopped");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool once = false;
    bool reset = false;
    bool status = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--once") {
            once = true;
        } else if (argument == "--reset") {
            reset = true;
        } else if (argument == "--status") {
            status = true;
        } else {
            std::fprintf(stderr, "usage: %s [--once | --reset | --status]\n",
                         argv[0]);
            return 2;
        }
    }

    Agent agent;
    if (status) {
        agent.step();
        agent.print_status();
        return 0;
    }
    if (reset) {
        agent.run_reset();
        return 0;
    }
    if (once) {
        const auto report = agent.step();
        if (!report) {
            return 0;
        }
        std::printf("%s\n", json::serialize(report_json(*report)).c_str());
        return report->errors.empty() ? 0 : 1;
    }
    return run_daemon(agent);
}
