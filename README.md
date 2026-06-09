<div align="center">

# 🧙 warlock-buddy

**A desk-side M5Stack CoreS3 command surface for the [Warlock](https://github.com/techmages-org/warlock) pentest deck — mirrors the engagement gate and lets you ARM / END / KILLSWITCH an offensive engagement with a physical tap.**

`hacker-HUD firmware` · `Bun BLE bridge` · `consumer-side only — no new deck surface`

</div>

---

## What it is

A CoreS3 sits on your desk and shows the live posture of the Warlock deck:

- **Hero gate banner** — `SAFE` / `ATTENTION` (engagements staged) / `ARMED` (live) / `OFFLINE`, in phosphor-green/amber/red with scanlines and corner brackets.
- **Telemetry grid** — active scope (ssid/bssid/cidr), **SIGILS** (count of signed AAR attestations), deck temp / cpu / mem / mesh / gps / sdr, operator + uplink heartbeat.
- **Tap-twice gate** — `ARM` a staged engagement, `END` the active one, or `KILLSWITCH` (abort all offensive ops). Every action is confirmed (tap again) and rides back over BLE to Warlock's **existing, audited** endpoints.

Built on the [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) NUS JSON wire protocol (cloned from the `subctl-buddy` pattern).

## Architecture

```
┌──────────────────┐   BLE NUS JSON    ┌───────────────────────┐   HTTP Basic   ┌──────────────┐
│  M5Stack CoreS3  │ ◀───────────────▶ │  bridge/ (Bun+noble)  │ ◀────────────▶ │ Warlock API  │
│  hacker HUD      │  heartbeat.warlock│  poll + map + gate     │  /api/...      │  (FastAPI)   │
│  ARM/END/KILL    │  {cmd:arm|end|..} │                        │                │  engagement  │
└──────────────────┘                   └───────────────────────┘                │  gate + AAR  │
                                                                                 └──────────────┘
```

**The gate is consumer-side only.** The bridge uses endpoints the Warlock TUI + web already use — `GET /api/dashboard/status`, `GET /api/engagements`, `POST /api/engagements/{id}/activate|end`, `POST /api/engagements/killswitch` — so it adds **zero** new backend surface.

## Repo layout

```
bridge/            Bun + noble BLE central daemon (TypeScript)
  src/warlock.ts     Warlock FastAPI client (reads + gate actions)
  src/protocol.ts    Warlock state → buddy heartbeat (warlock.* extension + persona)
firmware-cores3/   M5Stack CoreS3 firmware (PlatformIO/Arduino, M5Unified)
docs/              Hardware + Module-LLM (AX630C) research
```

## Quick start

```bash
# bridge — point it at your deck (defaults: 127.0.0.1:7777, warlock/warlock)
cd bridge && bun install
WARLOCK_API_URL=http://<deck>:7777 bun run src/cli.ts print     # heartbeat, no hardware
bun run src/cli.ts daemon --device warlock-XXXX                  # run the BLE bridge

# deck verbs (no hardware) — exercise the gate
bun run src/cli.ts engagements          # list (id / name / status)
bun run src/cli.ts arm <id>             # ARM a staged engagement
bun run src/cli.ts end <id> | killswitch

# firmware
cd firmware-cores3 && pio run -e m5stack-cores3        # build
pio run -e m5stack-cores3 -t upload                    # flash over USB
```

Bridge env: `WARLOCK_API_URL`, `WARLOCK_API_USER`, `WARLOCK_API_PASS`, `WARLOCK_OWNER_NAME`.

## Roadmap

- **Phase 3 — "ask the deck":** on-device LLM Q&A via the stacked **Module-LLM** (AX630C) over UART. See `docs/stack-and-llm-research.md`.
- **warlock-node:** the same stack as a networked AI recon brick (the Module-LLM is a full Ubuntu/aarch64 SBC with Ethernet via the LLM Mate).

## License

MIT.
