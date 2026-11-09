#!/usr/bin/env python3
"""WSL background throttle agent.

Driven by the Windhawk "Smart Process Priority & RAM Optimizer" mod running on
the Windows host. The mod writes state.json into
%LOCALAPPDATA%\\WslThrottle (reachable here as /mnt/c/Users/<you>/AppData/Local/
WslThrottle); this agent reconciles that policy and writes status.json back so
the mod can log the outcome.

Enforced knobs, all reversible:
  * cgroup v2 CPU quota on user.slice - caps interactive Linux CPU usage
  * docker update --cpus/--cpu-shares/--memory - per-container limits, with the
    original limits captured first and restored afterwards
  * drop_caches - page cache only, opt-in

The agent never treats file contents as commands: every field is parsed and
range-checked, and subprocesses are only ever spawned from fixed argv vectors.
"""

import errno
import fcntl
import glob
import json
import os
import subprocess
import sys
import time

RUN_DIR = "/run/wsl-throttle"
ORIGINALS_PATH = os.path.join(RUN_DIR, "originals.json")
LOCK_PATH = os.path.join(RUN_DIR, "lock")

STATE_GLOBS = ("/mnt/c/Users/*/AppData/Local/WslThrottle",)
POLL_SECONDS = float(os.environ.get("WSL_THROTTLE_POLL", "1"))
# If the mod stops refreshing state.json (unloaded, crashed, machine moved to
# another host), stop throttling rather than stranding the distro.
STALE_SECONDS = int(os.environ.get("WSL_THROTTLE_STALE", "300"))

MODE_THROTTLE = "throttle"
MODE_RESTORE = "restore"

# Written by install.sh from $WSL_DISTRO_NAME. When present, a policy addressed
# to a different distro is ignored instead of applied to the wrong one.
DISTRO_FILE = "/etc/wsl-throttle/distro"
_distro_cache = None


def _clean_token(value):
    """Mirror of the mod's SanitizeWslToken so both sides agree exactly."""
    if not isinstance(value, str):
        return ""
    keep = set(
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        ".-_ ()"
    )
    return "".join(ch for ch in value if ch in keep)[:64]


def own_distro_name():
    global _distro_cache
    if _distro_cache is None:
        name = ""
        try:
            with open(DISTRO_FILE, "r", encoding="utf-8") as handle:
                name = handle.read().strip()
        except OSError:
            pass
        if not name:
            name = os.environ.get("WSL_DISTRO_NAME", "")
        _distro_cache = _clean_token(name)
    return _distro_cache


def log(msg):
    sys.stdout.write("%s %s\n" % (time.strftime("%H:%M:%S"), msg))
    sys.stdout.flush()


def run(argv, timeout=25):
    try:
        proc = subprocess.run(
            argv, capture_output=True, text=True, timeout=timeout
        )
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except FileNotFoundError:
        return 127, "", "%s not found" % argv[0]
    except subprocess.TimeoutExpired:
        return 124, "", "%s timed out" % argv[0]
    except OSError as exc:
        return 1, "", "%s: %s" % (argv[0], exc)


# ---------------------------------------------------------------------------
# State file discovery and parsing
# ---------------------------------------------------------------------------


def find_state_dir():
    for pattern in STATE_GLOBS:
        for directory in sorted(glob.glob(pattern)):
            if os.path.isfile(os.path.join(directory, "state.json")):
                return directory
    return None


def _as_int(value, default, low, high):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return default
    return max(low, min(high, int(value)))


def load_state(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            raw = json.load(handle)
    except (OSError, ValueError) as exc:
        return None, "cannot read state: %s" % exc
    if not isinstance(raw, dict):
        return None, "state is not an object"

    mode = raw.get("mode")
    if mode not in (MODE_THROTTLE, MODE_RESTORE):
        return None, "unknown mode %r" % (mode,)

    docker = raw.get("docker") if isinstance(raw.get("docker"), dict) else {}
    state = {
        "mode": mode,
        "reason": str(raw.get("reason", ""))[:64],
        "generated": _as_int(raw.get("generated"), 0, 0, 2 ** 40),
        "cpu_quota_percent": _as_int(raw.get("cpu_quota_percent"), 35, 5, 90),
        "docker_enabled": bool(docker.get("enabled", True)),
        "docker_cpus_centi": _as_int(docker.get("cpus_centi"), 150, 10, 6400),
        "docker_memory_mb": _as_int(docker.get("memory_mb"), 0, 0, 262144),
        "drop_caches": bool(raw.get("drop_caches", False)),
        "distro": _clean_token(raw.get("distro")),
    }
    if state["mode"] == MODE_THROTTLE and STALE_SECONDS > 0:
        age = time.time() - state["generated"]
        if state["generated"] > 0 and age > STALE_SECONDS:
            state["mode"] = MODE_RESTORE
            state["reason"] = "state-stale"
    own = own_distro_name()
    if state["distro"] and own and state["distro"].lower() != own.lower():
        state["mode"] = MODE_RESTORE
        state["reason"] = "distro-mismatch"
    state["own_distro"] = own
    return state, None


def write_status(state_dir, payload):
    path = os.path.join(state_dir, "status.json")
    tmp = path + ".tmp"
    body = json.dumps(payload, separators=(",", ":"))
    try:
        with open(tmp, "w", encoding="utf-8") as handle:
            handle.write(body)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
        os.chmod(path, 0o644)
    except OSError as exc:
        log("status write failed: %s" % exc)


# ---------------------------------------------------------------------------
# cgroup / systemd
# ---------------------------------------------------------------------------


def systemd_available():
    return run(["systemctl", "is-system-running"], timeout=10)[0] in (0, 1) or os.path.isdir(
        "/run/systemd/system"
    )


def set_user_slice_quota(percent):
    spec = "CPUQuota=%d%%" % percent if percent else "CPUQuota="
    rc, _, err = run(["systemctl", "set-property", "--runtime", "user.slice", spec])
    if rc != 0:
        return "user.slice %s failed: %s" % (spec, err or "rc=%d" % rc)
    return None


# ---------------------------------------------------------------------------
# Docker
# ---------------------------------------------------------------------------

INSPECT_FORMAT = (
    "{{.Name}}|{{.HostConfig.NanoCpus}}|{{.HostConfig.CpuShares}}"
    "|{{.HostConfig.Memory}}|{{.HostConfig.MemorySwap}}"
)


def docker_running_ids():
    rc, out, err = run(["docker", "ps", "-q"], timeout=20)
    if rc != 0:
        return None, err or ("rc=%d" % rc)
    return [line.strip() for line in out.splitlines() if line.strip()], None


def docker_inspect(ids):
    if not ids:
        return [], None
    rc, out, err = run(["docker", "inspect", "-f", INSPECT_FORMAT] + list(ids), timeout=40)
    if rc != 0:
        return [], err or ("rc=%d" % rc)
    records = []
    for index, line in enumerate(out.splitlines()):
        fields = line.split("|")
        if len(fields) != 5:
            continue
        records.append(
            {
                "id": ids[index] if index < len(ids) else None,
                "name": fields[0].lstrip("/"),
                "nano": int(fields[1] or 0),
                "shares": int(fields[2] or 0),
                "mem": int(fields[3] or 0),
                "swap": int(fields[4] or 0),
            }
        )
    return records, None


def host_cpu_count():
    try:
        return max(1, len(os.sched_getaffinity(0)))
    except AttributeError:
        return max(1, os.cpu_count() or 1)


def host_ram_bytes():
    try:
        with open("/proc/meminfo", "r", encoding="ascii") as handle:
            for line in handle:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        pass
    return 8 * 1024 * 1024 * 1024


def apply_container_limit(cid, cpus_centi, memory_mb):
    argv = [
        "docker",
        "update",
        "--cpus",
        "%.2f" % (cpus_centi / 100.0),
        "--cpu-shares",
        "256",
    ]
    if memory_mb > 0:
        argv += [
            "--memory",
            "%dm" % memory_mb,
            "--memory-swap",
            "%dm" % (memory_mb * 2),
        ]
    rc, _, err = run(argv + [cid], timeout=40)
    return (err or "rc=%d" % rc) if rc != 0 else None


def restore_container_limit(cid, original):
    cpus = original["nano"] / 1e9 if original["nano"] > 0 else float(host_cpu_count())
    argv = [
        "docker",
        "update",
        "--cpus",
        "%.2f" % cpus,
        "--cpu-shares",
        str(original["shares"] if original["shares"] > 0 else 1024),
    ]
    if original["mem_applied"]:
        mem = original["mem"] if original["mem"] > 0 else host_ram_bytes()
        swap = original["swap"] if original["swap"] > 0 else mem * 2
        argv += ["--memory", str(mem), "--memory-swap", str(swap)]
    rc, _, err = run(argv + [cid], timeout=40)
    return (err or "rc=%d" % rc) if rc != 0 else None


# ---------------------------------------------------------------------------
# Agent
# ---------------------------------------------------------------------------


class Agent:
    def __init__(self):
        self.state_dir = None
        self.applied = None          # signature of the currently applied policy
        self.tracked = {}            # container id -> original limits
        self.last_errors = []
        self.last_status_signature = None
        self.load_originals()

    # -- persistence -------------------------------------------------------

    def load_originals(self):
        try:
            with open(ORIGINALS_PATH, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            return
        if isinstance(data, dict):
            self.tracked = data

    def save_originals(self):
        try:
            os.makedirs(RUN_DIR, exist_ok=True)
            tmp = ORIGINALS_PATH + ".tmp"
            with open(tmp, "w", encoding="utf-8") as handle:
                json.dump(self.tracked, handle)
            os.replace(tmp, ORIGINALS_PATH)
        except OSError as exc:
            log("originals write failed: %s" % exc)

    # -- policy ------------------------------------------------------------

    def reconcile(self, state):
        signature = (
            state["mode"],
            state["cpu_quota_percent"] if state["mode"] == MODE_THROTTLE else 0,
            state["docker_enabled"] if state["mode"] == MODE_THROTTLE else False,
            state["docker_cpus_centi"],
            state["docker_memory_mb"],
        )
        errors = []
        changed = signature != self.applied
        throttling = state["mode"] == MODE_THROTTLE

        if changed:
            if not systemd_available():
                errors.append("systemd not available, cannot set cgroup quota")
            else:
                problem = set_user_slice_quota(
                    state["cpu_quota_percent"] if throttling else 0
                )
                if problem:
                    errors.append(problem)

        container_note = self.reconcile_containers(state, throttling, changed, errors)

        if throttling and changed and state["drop_caches"]:
            run(["sync"], timeout=30)
            try:
                with open("/proc/sys/vm/drop_caches", "w", encoding="ascii") as handle:
                    handle.write("3")
            except OSError as exc:
                errors.append("drop_caches: %s" % exc)

        self.applied = signature if not errors else None
        self.last_errors = errors
        return {
            "mode": state["mode"],
            "reason": state["reason"],
            "cpu_quota_percent": state["cpu_quota_percent"] if throttling else 0,
            "containers": container_note,
            "distro": state.get("own_distro", ""),
            "errors": errors,
        }

    def reconcile_containers(self, state, throttling, changed, errors):
        ids, problem = docker_running_ids()
        if ids is None:
            if state["docker_enabled"]:
                errors.append("docker: %s" % problem)
            return 0

        # Drop containers that have exited.
        for cid in [c for c in self.tracked if c not in ids]:
            del self.tracked[cid]
            changed = True

        if not throttling:
            if not self.tracked:
                return 0
            restored = 0
            for cid, original in list(self.tracked.items()):
                if cid in ids:
                    problem = restore_container_limit(cid, original)
                    if problem:
                        errors.append("restore %s: %s" % (original.get("name", cid[:12]), problem))
                        continue
                    restored += 1
                del self.tracked[cid]
            self.save_originals()
            if restored:
                log("restored limits on %d container(s)" % restored)
            return restored

        if not state["docker_enabled"]:
            return 0

        records, problem = docker_inspect(ids)
        if problem:
            errors.append("docker inspect: %s" % problem)
            return 0

        # docker inspect emits one line per container, in argument order.
        throttled = 0
        for record in records:
            cid = record["id"]
            tracked = self.tracked.get(cid)
            if tracked is None:
                tracked = {
                    "name": record["name"],
                    "nano": record["nano"],
                    "shares": record["shares"],
                    "mem": record["mem"],
                    "swap": record["swap"],
                    "mem_applied": state["docker_memory_mb"] > 0,
                }
                self.tracked[cid] = tracked
                self.save_originals()
                changed = True
            if not changed and tracked.get("limited"):
                continue
            problem = apply_container_limit(
                cid, state["docker_cpus_centi"], state["docker_memory_mb"]
            )
            if problem:
                errors.append("limit %s: %s" % (tracked.get("name", cid[:12]), problem))
                continue
            tracked["limited"] = True
            throttled += 1
        return throttled

    # -- main loop ---------------------------------------------------------

    def step(self, force_state=None):
        directory = self.state_dir or find_state_dir()
        if directory is None:
            return None
        self.state_dir = directory
        state, problem = load_state(os.path.join(directory, "state.json"))
        if state is None:
            if self.last_status_signature != problem:
                self.last_status_signature = problem
                write_status(directory, {
                    "agent": "error",
                    "error": problem,
                    "updated": int(time.time()),
                })
            return None
        report = self.reconcile(state)
        # Only touch status.json when the outcome changes, so the mod logs
        # transitions instead of one line per poll.
        signature = json.dumps(report, sort_keys=True)
        if signature != self.last_status_signature:
            self.last_status_signature = signature
            write_status(directory, dict(report, agent="ok", updated=int(time.time())))
        return report


def acquire_lock():
    os.makedirs(RUN_DIR, exist_ok=True)
    handle = open(LOCK_PATH, "w", encoding="ascii")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        if exc.errno in (errno.EACCES, errno.EAGAIN):
            return None
        raise
    return handle


def main(argv):
    agent = Agent()

    if "--status" in argv:
        state_dir = find_state_dir()
        print(json.dumps({
            "state_dir": state_dir,
            "applied": agent.applied,
            "tracked_containers": len(agent.tracked),
        }, indent=2))
        return 0

    if "--once" in argv or "--reset" in argv:
        if "--reset" in argv:
            state_dir = find_state_dir()
            if state_dir:
                write_status(state_dir, {
                    "agent": "ok", "mode": MODE_RESTORE,
                    "reason": "manual-reset", "containers": len(agent.tracked),
                    "errors": [], "updated": int(time.time()),
                })
            agent.applied = None
            report = agent.reconcile({
                "mode": MODE_RESTORE, "reason": "manual-reset", "generated": int(time.time()),
                "cpu_quota_percent": 0, "docker_enabled": True, "docker_cpus_centi": 100,
                "docker_memory_mb": 0, "drop_caches": False,
            })
        else:
            report = agent.step()
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0 if not (report and report.get("errors")) else 1

    lock = acquire_lock()
    if lock is None:
        log("another agent instance is running")
        return 0

    log("started (poll %.1fs, stale-after %ds)" % (POLL_SECONDS, STALE_SECONDS))
    last_signature = None
    while True:
        try:
            report = agent.step()
            if report is not None:
                signature = (report["mode"], report["reason"], tuple(report["errors"]))
                if signature != last_signature:
                    last_signature = signature
                    if report["errors"]:
                        log("mode=%s reason=%s errors=%s"
                            % (report["mode"], report["reason"], "; ".join(report["errors"])))
                    else:
                        log("mode=%s reason=%s containers=%s"
                            % (report["mode"], report["reason"], report["containers"]))
        except Exception as exc:  # keep the service alive across bad state
            log("reconcile failed: %s" % exc)
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
