#============================================================================================================
#
#                   Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
#                               SPDX-License-Identifier: BSD-3-Clause
#
#============================================================================================================

#!/usr/bin/env bash
# ==============================================================================
# Open an SSH session (or run a command) on the QDC IoT device through the
# localhost:2222 tunnel, supplying the device root password automatically so
# you are never prompted.
#
# The password is read from config.local.sh (IOT_TARGET_PASSWORD) in this dir,
# or from the IOT_TARGET_PASSWORD environment variable.
#
# Usage:
#   ./qdc-ssh.sh                 # interactive shell on the device
#   ./qdc-ssh.sh uname -a        # run a single command
#   ./qdc-ssh.sh "rm -rf /tmp/SnapdragonProfiler"
#
# Prerequisite: the QDC SSH tunnel to the device must be up, e.g.
#   ssh -i ~/.ssh/id_ed25519 -L 2222:<device>.sa.svc.cluster.local:22 -N sshtunnel@ssh.qdc.qualcomm.com
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/config.local.sh" ]]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/config.local.sh"
fi

PORT="${IOT_LOCAL_SSH_PORT:-2222}"
USER_HOST="${IOT_TARGET_USER:-root}@localhost"
PW="${IOT_TARGET_PASSWORD:-}"

# Force password auth (the SSH key is not accepted on the device) and skip host-key
# checks (the tunnel port is reused across QDC sessions with changing host keys).
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
      -o PreferredAuthentications=password -o PubkeyAuthentication=no -p "$PORT")

if [[ -z "$PW" ]]; then
    echo "IOT_TARGET_PASSWORD is not set (config.local.sh); ssh will prompt for the password." >&2
    exec ssh "${OPTS[@]}" "$USER_HOST" "$@"
fi

if command -v sshpass &>/dev/null; then
    export SSHPASS="$PW"
    exec sshpass -e ssh "${OPTS[@]}" "$USER_HOST" "$@"
else
    # No sshpass: feed the password via SSH_ASKPASS. ssh only uses the askpass helper
    # for a password prompt when it has no controlling terminal, so run it under setsid.
    # For an interactive session (no command args) add -tt so a remote pty is still allocated.
    ASKPASS="$(mktemp "${TMPDIR:-/tmp}/qdc-askpass.XXXXXX")"
    printf '#!/usr/bin/env bash\nprintf "%%s\\n" "$IOT_TARGET_PASSWORD"\n' > "$ASKPASS"
    chmod +x "$ASKPASS"
    export IOT_TARGET_PASSWORD="$PW"
    export SSH_ASKPASS="$ASKPASS"
    export SSH_ASKPASS_REQUIRE=force
    trap 'rm -f "$ASKPASS"' EXIT
    if [[ $# -eq 0 ]]; then
        setsid -w ssh -tt "${OPTS[@]}" "$USER_HOST"
    else
        setsid -w ssh "${OPTS[@]}" "$USER_HOST" "$@"
    fi
fi
