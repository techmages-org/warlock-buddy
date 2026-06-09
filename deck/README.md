# Deck-side bridge (run the bridge *on* Warlock)

The cleanest deployment: run the warlock-buddy bridge **on the Warlock deck
itself** so the CoreS3 buddy pairs straight to the deck — no laptop in the loop.

```
CoreS3 buddy  ◀──BLE (CM5 onboard, hci0)──▶  Warlock deck  ──localhost:7777──▶  Warlock API
                                              (AC1200 MT7921 stays free for monitor mode)
```

## Why this works
The deck has Bluetooth two ways:
- **CM5 onboard** — Cypress CYW43455, **BT 5.0 / BLE** (the wireless CM5 SKU). Usually `hci0`.
- **AC1200 card** — MediaTek **MT7921AUN**, Wi-Fi 6E **+ BT 5.2** (lower IPEX pair = the BT antenna). Usually `hci1`.

Use the **CM5 onboard** radio for the buddy (`NOBLE_HCI_DEVICE_ID=0`) and the
AC1200 keeps doing monitor-mode/injection Wi-Fi. Clean separation.

## Install

On the deck (Raspberry Pi OS / Debian, ARM64), from the repo root:

```bash
sudo deck/install.sh
```

It installs BlueZ + a build toolchain, ensures Bun, runs `bun install` (building
noble's native HCI binding), writes `/etc/warlock-buddy/bridge.env`, and installs
+ starts a `warlock-buddy` **systemd** service. The service grabs raw HCI via
`AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN` (no root needed).

## Configure

Edit `/etc/warlock-buddy/bridge.env`, then `sudo systemctl restart warlock-buddy`:

| Var | Default | Notes |
|---|---|---|
| `WARLOCK_API_URL` | `http://127.0.0.1:7777` | the deck's own API |
| `WARLOCK_API_USER` / `_PASS` | `warlock` / `warlock` | HTTP Basic creds |
| `WARLOCK_OWNER_NAME` | install user | shown as `OP` on the HUD |
| `NOBLE_HCI_DEVICE_ID` | `0` | `0` = CM5 onboard, `1` = AC1200 BT |
| `WARLOCK_BUDDY_DEVICE` | (empty) | pin to `warlock-XXXX`; empty = first seen |

## Operate

```bash
journalctl -u warlock-buddy -f        # logs (look for "connected: warlock-XXXX")
sudo systemctl restart warlock-buddy  # after env edits
sudo deck/uninstall.sh                # remove the service
```

## ⚠️ Not yet hardware-verified
Written against the documented stack; **validate on the deck on first boot.**
Known things to check:
- **bluetoothd coexistence.** noble (`bluetooth-hci-socket`) opens an HCI raw
  socket; the adapter must be `up`. The unit runs `hciconfig hciN up` first. If
  scanning never sees the buddy, confirm the right `hciN` (`hciconfig -a`), or
  dedicate the adapter (`sudo systemctl stop bluetooth` / use the AC1200's BT
  for the buddy and leave onboard to bluetoothd).
- **Adapter numbering** (`hci0`/`hci1`) can swap by probe order — verify with
  `hciconfig -a` and set `NOBLE_HCI_DEVICE_ID` accordingly.
- **noble native build** needs `libbluetooth-dev` + `build-essential` (installed
  by the script).
