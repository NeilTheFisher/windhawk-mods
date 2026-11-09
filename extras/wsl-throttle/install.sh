#!/usr/bin/env bash
# Install / update the WSL throttle agent. Run as root inside the distro.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ${EUID} -ne 0 ]]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi

command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }

install -d -m 0755 /usr/local/share/wsl-throttle
install -m 0755 "${here}/agent.py" /usr/local/bin/wsl-throttle-agent
install -m 0644 "${here}/wsl-throttle.service" /etc/systemd/system/wsl-throttle.service
install -m 0644 "${here}/README.md" /usr/local/share/wsl-throttle/README.md 2>/dev/null || true

# Record which distro this is, so a policy published for a different one can be
# ignored. Only available when install.sh runs from a WSL shell.
install -d -m 0755 /etc/wsl-throttle
if [[ -n "${WSL_DISTRO_NAME:-}" ]]; then
    printf '%s\n' "${WSL_DISTRO_NAME}" > /etc/wsl-throttle/distro
    chmod 0644 /etc/wsl-throttle/distro
    echo "distro recorded as: ${WSL_DISTRO_NAME}"
else
    echo "WSL_DISTRO_NAME is empty; the agent will accept any distro's policy" >&2
fi

systemctl daemon-reload
systemctl enable wsl-throttle.service >/dev/null
systemctl restart wsl-throttle.service
sleep 1
systemctl --no-pager --lines=5 status wsl-throttle.service || true
