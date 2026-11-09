#!/usr/bin/env bash
# Remove the WSL throttle agent and restore any limits it applied.
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

if [[ -x /usr/local/bin/wsl-throttle-agent ]]; then
    /usr/local/bin/wsl-throttle-agent --reset || true
fi

systemctl disable --now wsl-throttle.service 2>/dev/null || true
rm -f /etc/systemd/system/wsl-throttle.service /usr/local/bin/wsl-throttle-agent
rm -rf /usr/local/share/wsl-throttle /run/wsl-throttle /etc/wsl-throttle
systemctl daemon-reload
echo "removed"
