#!/usr/bin/env bash
# warlock-buddy — deck-side bridge installer (Linux / Raspberry Pi OS, ARM64).
#
# Runs the BLE bridge ON the Warlock deck itself, so the CoreS3 buddy pairs
# straight to Warlock — no second machine in the loop. Uses the deck's onboard
# Bluetooth (CM5 CYW43455, hci0) by default, leaving the AC1200 MT7921 free for
# monitor-mode Wi-Fi.
#
#   sudo deck/install.sh
#
# Idempotent: re-run after edits. Honors env overrides (see bridge.env below).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_DIR="$REPO_DIR/bridge"
ENV_DIR="/etc/warlock-buddy"
ENV_FILE="$ENV_DIR/bridge.env"
UNIT="/etc/systemd/system/warlock-buddy.service"

# The non-root user the service runs as (must own the repo / can build).
DECK_USER="${WARLOCK_BUDDY_USER:-${SUDO_USER:-$(id -un)}}"
DECK_HOME="$(eval echo "~$DECK_USER")"

say() { printf '\033[1;32m▌\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || die "deck installer is Linux-only (this is $(uname -s)). On macOS use 'warlock-buddy install'."
[[ $EUID -eq 0 ]] || die "run with sudo (needs apt + systemd)."

say "deck user      : $DECK_USER ($DECK_HOME)"
say "repo           : $REPO_DIR"

# 1. Build deps for @abandonware/bluetooth-hci-socket (noble's native HCI binding) + BlueZ.
say "installing apt deps (bluez, libbluetooth-dev, build toolchain)…"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  bluez libbluetooth-dev build-essential python3 ca-certificates curl >/dev/null
say "apt deps ok"

# 2. Bun for the deck user (official installer; arm64 supported).
BUN="$DECK_HOME/.bun/bin/bun"
if [[ ! -x "$BUN" ]]; then
  if command -v bun >/dev/null 2>&1; then
    BUN="$(command -v bun)"
  else
    say "installing bun for $DECK_USER…"
    sudo -u "$DECK_USER" bash -c 'curl -fsSL https://bun.sh/install | bash' >/dev/null
  fi
fi
[[ -x "$BUN" ]] || die "bun not found at $BUN after install"
say "bun            : $BUN"

# 3. Install JS deps + build the native HCI binding (as the deck user).
say "bun install (builds @abandonware/bluetooth-hci-socket native addon)…"
sudo -u "$DECK_USER" bash -c "cd '$BRIDGE_DIR' && '$BUN' install" \
  || die "bun install failed — check build toolchain / network"
say "bridge deps ok"

# 4. Env file (created once; edit it, then 'systemctl restart warlock-buddy').
mkdir -p "$ENV_DIR"
if [[ ! -f "$ENV_FILE" ]]; then
  cat > "$ENV_FILE" <<EOF
# warlock-buddy bridge — deck config. Restart after edits:
#   sudo systemctl restart warlock-buddy
WARLOCK_API_URL=http://127.0.0.1:7777
WARLOCK_API_USER=warlock
WARLOCK_API_PASS=warlock
WARLOCK_OWNER_NAME=$DECK_USER
# BLE adapter: 0 = CM5 onboard (recommended; leaves AC1200 MT7921 for monitor mode).
NOBLE_HCI_DEVICE_ID=0
# Pin to one device (optional). Empty = first 'warlock-*' peripheral seen.
WARLOCK_BUDDY_DEVICE=
EOF
  say "wrote $ENV_FILE (defaults — edit creds/adapter as needed)"
else
  say "kept existing $ENV_FILE"
fi

# 5. systemd unit. AmbientCapabilities grants raw HCI access without running as
#    root; ExecStartPre brings the chosen adapter up. After warlock.service so
#    the API is live first (Wants, not Requires — the buddy shows OFFLINE if not).
say "writing $UNIT…"
cat > "$UNIT" <<EOF
[Unit]
Description=warlock-buddy BLE bridge (deck -> CoreS3 companion)
After=bluetooth.target network-online.target warlock.service
Wants=bluetooth.target

[Service]
Type=simple
User=$DECK_USER
EnvironmentFile=$ENV_FILE
# bring the chosen HCI adapter up before noble grabs it
ExecStartPre=-/usr/bin/hciconfig hci\${NOBLE_HCI_DEVICE_ID} up
ExecStart=$BUN run $BRIDGE_DIR/src/cli.ts daemon
Restart=on-failure
RestartSec=5
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable warlock-buddy.service >/dev/null 2>&1 || true
systemctl restart warlock-buddy.service

say "installed + started. status:"
sleep 1
systemctl --no-pager --lines=8 status warlock-buddy.service || true
cat <<EOF

next:
  • add a Bluetooth antenna to the AC1200's lower IPEX pair if you use hci1
  • find the buddy's name:   journalctl -u warlock-buddy -f   (look for 'connected: warlock-XXXX')
  • pin it:                  edit WARLOCK_BUDDY_DEVICE in $ENV_FILE, then
                             sudo systemctl restart warlock-buddy
  • logs:                    journalctl -u warlock-buddy -f
  • remove:                  sudo deck/uninstall.sh
EOF
