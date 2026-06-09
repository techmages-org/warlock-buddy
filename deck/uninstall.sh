#!/usr/bin/env bash
# warlock-buddy — remove the deck-side bridge service. Leaves the repo + env file.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }

systemctl stop warlock-buddy.service 2>/dev/null || true
systemctl disable warlock-buddy.service 2>/dev/null || true
rm -f /etc/systemd/system/warlock-buddy.service
systemctl daemon-reload
echo "▌ warlock-buddy service removed."
echo "  (config kept at /etc/warlock-buddy/bridge.env — rm it manually to wipe)"
