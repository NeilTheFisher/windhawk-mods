# wsl-throttle

Root agent inside the WSL distro that applies the background-throttle policy
published by the Windhawk **Smart Process Priority & RAM Optimizer** mod.

The mod runs in the Windows user session and cannot see or touch Linux PIDs, and
`vmmemWSL` is hypervisor-owned, so Windows-side throttling of WSL processes is
not possible. The mod therefore only decides policy and writes a small JSON
file; this agent enforces it and reports back.

## Files

| Path | Purpose |
| --- | --- |
| `agent.cpp` | Policy parsing, reconcile loop, status reporting |
| `docker.cpp` | Everything that knows the docker CLI: limits, apply, grouped restore |
| `sys.cpp` | Subprocess, file and host primitives |
| `json.cpp` | Small JSON reader/writer (the state files are the only input) |
| `Makefile` | `make` builds `wsl-throttle-agent` |
| `wsl-throttle.service` | systemd unit, runs as root in `system.slice` |
| `install.sh` | Build + install/update (must run as root) |
| `uninstall.sh` | Stop, restore limits, remove |

Installed locations: `/usr/local/bin/wsl-throttle-agent` and
`/etc/systemd/system/wsl-throttle.service`.

## Build

Needs a C++17 compiler and `make` (`apt install build-essential`). The binary is
linked against a static libstdc++/libgcc, so at runtime it only needs libc.

```bash
make                    # -> ./wsl-throttle-agent, ~360 KB
sudo ./install.sh       # builds if needed, then installs and restarts
```

## Cost

The agent is deliberately cheap, because it runs forever:

- **Steady state**: one `stat()` per second on `state.json`. An unchanged policy
  costs no read, no parse and no subprocess - the reconcile is skipped entirely
  because the applied state still matches.
- **Transitions**: one `systemctl` call plus one `docker update`, regardless of
  how many containers there are, because `docker update` takes a list. On this
  machine 16 containers throttle in ~0.5s and restore in ~0.3s, against ~1.2s
  for the per-container loop it replaced.
- **Container discovery**: `docker ps` runs at most once every 15s, so a new
  container is still picked up quickly without polling docker every second.
- **Memory**: ~2.5MB resident (the Python agent it replaced held ~7.5MB).

## Channel

The mod writes `state.json` to `%LOCALAPPDATA%\WslThrottle`, which is
`/mnt/c/Users/<you>/AppData/Local/WslThrottle` here. The agent finds it by
globbing `/mnt/c/Users/*/AppData/Local/WslThrottle`, so a username change or a
second profile needs no reconfiguration.

`state.json` (written by the mod, schema v1):

```json
{
  "version": 1,
  "mode": "throttle",
  "reason": "wsl-background",
  "generated": 1794204671,
  "cpu_quota_percent": 35,
  "docker": { "enabled": true, "cpus_centi": 150, "memory_mb": 0 },
  "drop_caches": false,
  "distro": ""
}
```

Every field is range-checked and only fixed argv vectors are spawned; nothing in
the file is ever interpreted as a command or a shell fragment.

`status.json` is written back (same directory) whenever the outcome changes, and
the mod logs it as `[WSL] ...`.

## What is enforced

- **WSL processes** - `systemctl set-property --runtime user.slice
  CPUQuota=<n>%`. Covers all interactive Linux processes. Docker and other
  system services live in `system.slice` and are unaffected, which is why
  containers need the separate path below.
- **Docker containers** - `docker update --cpus <n> --cpu-shares 256` (and
  `--memory` only if `memory_mb > 0`). Original limits are captured first and
  restored on release. New containers started while throttled are picked up on
  the next poll.
- **Page cache** - opt-in `sync` + `drop_caches` on the transition into
  throttling. This releases cache pages only; it never touches process memory.

Not throttled: root/system services, the agent itself, and anything the mod
never publishes.

## When it goes back to normal

Throttling is a state, not a one-way switch. The mod re-evaluates every worker
tick (10s) and publishes a restore the moment any of these become true, so the
machine returns to full speed without a reboot, a service restart or manual
intervention:

| Trigger | `reason` | Timing |
| --- | --- | --- |
| A WSL host window is focused (terminal, WSLg app, `wsl.exe`) | `wsl-foreground` | immediately |
| A game is running (the mod's own gaming lockdown) | `gaming` | immediately |
| **The game closes** | `wsl-background` | next tick, back to throttling |
| On battery | `battery` | immediately |
| Back on AC power | `wsl-background` | after the grace period |
| System suspends | `suspended` | immediately, before sleep |
| System resumes | `wsl-foreground` | then the normal grace period |
| Steam Deck / resource hog boots | `gaming` | immediately |
| `enableWslThrottling` turned off | `disabled` | immediately |
| Mod unloaded or Windhawk exits | `uninit` | immediately |
| `state.json` not refreshed for 300s (mod crashed) | `state-stale` | within 5 min |

A game exiting is the interesting case: gaming lockdown is re-checked on every
tick, so the moment its trailing grace period lapses the mod publishes
`wsl-background` again and the cap comes back. Nothing has to be clicked.

## Tests

`make test` builds and runs the unit tests (no root, no docker, no Windows side
needed). They cover the places where a silent bug would strand a container:

- JSON parsing: the exact state file the mod writes, malformed input, escapes,
  and number clamping.
- Policy: throttle/restore, unknown mode rejected, missing fields falling back to
  safe defaults, out-of-range values clamped, the distro gate (including
  case-insensitivity), staleness, and the applied-state signature.
- Docker: the exact `docker update` argv, memory/swap handling, and the limit
  grouping that decides how many calls a transition costs.
- Host primitives: splitting and trimming the CLI output that gets parsed.

## Choosing a distro

`wslDistroName` defaults to empty, which means "whichever distro runs the
agent" - the normal single-distro case, and the recommended setting.

Set it (e.g. `Ubuntu`) only if you run the agent in more than one distro and
want the policy to land in exactly one. The name must match what
`wsl.exe -l -v` reports (comparison ignores case, so `ubuntu` is fine);
`install.sh` records it automatically from `WSL_DISTRO_NAME` into
`/etc/wsl-throttle/distro`. An agent whose recorded name differs from the
policy's ignores the policy and restores instead (`reason: distro-mismatch`).
Nothing about the distro is compiled in: if `/etc/wsl-throttle/distro` is
missing and `WSL_DISTRO_NAME` is unset, the agent accepts any policy.

Getting the two sides out of sync is the one failure mode worth knowing: if the
agent records `Ubuntu-22.04` while the real name is `Ubuntu`, every policy will
be dropped as a mismatch. `wsl.exe -l -v` is the source of truth.

## Failsafes

- `state.json` older than `WSL_THROTTLE_STALE` (default 300s) while in throttle
  mode means the mod is gone: the agent restores instead of stranding the distro.
- `systemctl set-property --runtime` writes to `/run`, so nothing survives a
  reboot.
- Container originals are kept in `/run/wsl-throttle/originals.json` so a service
  restart can still roll back.

## Manual use

```bash
sudo wsl-throttle-agent --status          # effective state
sudo wsl-throttle-agent --once            # one reconcile pass, prints the report
sudo wsl-throttle-agent --reset           # restore everything now
journalctl -u wsl-throttle -f             # follow transitions
```

## Known limits

- Memory limits cannot be cleared back to "unlimited" through `docker update`
  (`--memory 0` is a no-op, `--memory -1` is rejected), so a restored container
  whose original limit was unset gets the host's physical RAM as its limit. CPU
  and cpu-shares restore exactly.
- `cpu-shares` restores to `1024` when the original was unset (kernel default,
  functionally identical to unset).
- Requires cgroup v2 and systemd in the distro (both present on current WSL2
  Ubuntu). Without systemd the agent reports the error in `status.json`.
