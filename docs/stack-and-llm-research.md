# Warlock-buddy stack — hardware + LLM research

> Compiled 2026-06-09. Feeds the warlock-buddy build (esp. Phase 3, Module-LLM).
> Every non-obvious spec is sourced; see the Sources section.

## The physical stack (bottom → top)

| Layer | Product (best match) | What it adds |
|---|---|---|
| Base | **M5Stack DinBase** (SKU M132) | DIN-rail mount + LEGO holes, 9–24 V industrial DC input w/ hard power switch, **500 mAh** cell, PORT.B + PORT.C broken out, proto area |
| Battery | **Module13.2 Battery** (1500 mAh) | Pure capacity; stacks/parallels for more runtime |
| LLM carrier | **Module13.2 LLM Mate** | **RJ45 100 M Ethernet** (network transformer), **USB-C serial console** (CH340N), FPC-8P direct link to the LLM module, HT3.96 DIY pads |
| AI compute | **Module-LLM (AXERA AX630C)** | On-device Linux AI: LLM / VLM / ASR / TTS / KWS — talks to host over UART |
| Host | **M5Stack CoreS3 (ESP32-S3)** | The brains/UI: touchscreen, camera, mic, speaker, WiFi/BLE, IMU |

⚠️ **One thing to confirm physically:** is the battery layer the thin **Module13.2 (1500 mAh)**, or the magnetic **M5GO Bottom3 (500 mAh)**? It matters because the **Bottom3 has an IR-LED emitter** — it would give the stack IR that the CoreS3 itself lacks. Look for RGB LEDs + a magnetic pogo base (= Bottom3) vs. a plain thin module (= 13.2).

---

## CoreS3 — capability matrix (the host)

| Capability | Status | Part / detail |
|---|---|---|
| SoC | ✅ | ESP32-S3, dual Xtensa LX7 @ 240 MHz |
| Flash / PSRAM | ✅ | 16 MB flash / 8 MB PSRAM |
| WiFi | ✅ | 2.4 GHz only (WiFi 4) — no 5 GHz |
| Bluetooth | ✅ BLE 5 / ❌ Classic | no A2DP/BT-Classic |
| Display | ✅ | 2.0″ IPS 320×240, ILI9342C |
| Touch | ✅ | capacitive, FT6336U |
| **Camera** | ✅ | **GC0308**, VGA (640×480), 8-bit DVP |
| **Microphone** | ✅ ×2 | dual MEMS → **ES7210** ADC over I2S |
| **Speaker** | ✅ | **AW88298** I2S amp + 1 W speaker |
| IMU | ✅ | BMI270 (6-axis) + BMM150 mag = 9-DOF |
| PMIC / GPIO-exp / RTC | ✅ | AXP2101 / AW9523B / BM8563 |
| Prox + ambient light | ✅ | LTR-553ALS |
| microSD | ✅ | TF slot (≤16 GB per docs) |
| USB-C | ✅ | native USB-OTG + CDC |
| Grove | ✅ | PORT.A I2C / PORT.B GPIO-ADC / **PORT.C UART (G17/G18)** |
| **IR TX/RX** | ❌ | **NONE on CoreS3** (Core/Fire had it; Core2 dropped it; CoreS3 also omits it) |
| Tactile buttons | ❌ | replaced by touch zones (power btn only) |
| Ethernet / RS485 / GPS / LoRa | ❌ | need a module/base |

**IR answer (your direct question):** the CoreS3 has **no IR transmitter and no IR receiver**. Options to add it: a Grove **Mini IR Unit** (TX+RX), or confirm the bottom is the **M5GO Bottom3** (has an IR emitter).

**Audio/camera init note:** all sensors share one internal I2C bus (G11/G12); power/reset to LCD, camera, and audio is gated through AXP2101 + AW9523B, so PMIC/expander must init first (M5Unified handles this).

---

## Module-LLM (AX630C) — the on-device AI

**It's a Linux SBC-on-a-module, not an MCU peripheral.** Runs Ubuntu + the **StackFlow** service; the CoreS3 is a *client* over UART.

| Item | Spec |
|---|---|
| SoC | AXERA AX630C |
| CPU | dual Cortex-A53 @ 1.2 GHz (aarch64) |
| NPU | **3.2 TOPS @ INT8** (up to 12.8 @ INT4), native Transformer |
| RAM | 4 GB LPDDR4 → **~1 GB system / ~3 GB NPU** |
| Storage | 32 GB eMMC 5.1 (OS + models under `/opt/m5stack/`) |
| Mic / speaker | MEMS mic MSM421A / AW8737 amp + 1 W speaker (on-module) |
| Camera | **none** — VLM uses the *CoreS3's* camera (host sends JPEG over UART) |
| Pre-installed | **Qwen2.5-0.5B** + KWS + ASR + TTS |

### Host ↔ module wire protocol (StackFlow over UART) — firmware-facing
- **Physical:** UART **115200 8N1** on CoreS3 **PORT.C** (host RX=G18, TX=G17). Raise baud for VLM (JPEGs are slow at 115200).
- **Framing:** newline-delimited JSON. Request: `{request_id, work_id, action, object, data}`. Response adds `created` + `error{code,message}`.
- **`work_id` is the key concept:** on `setup` you send the bare unit name (`"llm"`); the module returns an instance id (`"llm.1003"`) you must reuse on every later call.
- **Boot sequence:** `sys ping` (poll until OK) → `sys reset` (wait for `"reset over"`) → setup units. Discard the raw `V0EUEURS` banner after a reboot.
- **LLM path:** `llm.setup {model, response_format:"llm.utf-8.stream", max_token_len:127, prompt:"<system>"}` → `inference {delta, index, finish:true}` → **stream back frames; accumulate `data.delta` until `data.finish==true`** (the only authoritative EOS).
- **Gotcha:** JSON key is `max_token_len` (the doc prose mislabels it `max_length`).
- **Built-in units:** `sys, audio, kws, asr, whisper, vad, llm, vlm, tts, melotts, camera, yolo, depth_anything`. A high-level `M5ModuleLLM_VoiceAssistant` chains **KWS→ASR→LLM→TTS** with no host round-trips.

### Two ways to drive it (the Mate changes the game)
1. **Over UART from the CoreS3** (classic): CoreS3 is the operator UI; sends prompts, renders tokens. Decoupled from BLE/Warlock.
2. **Over Ethernet via the LLM Mate**: the module is a network node — SSH in to manage/install models, OR have it hit Warlock's FastAPI directly. Opens an "ask the deck" path that doesn't bottleneck on the CoreS3 UART. StackFlow also exposes an **OpenAI-compatible API** — so the module can be a drop-in local OpenAI endpoint on the LAN.

### Models — install & switch
- apt repo (`repo.llm.m5stack.com`), packages `llm-model-<name>-p256-ax630c`. `apt install …` then restart the service.
- Host selects model via `sys lsmode` (list) → pass `"model"` in `setup`. Switch = `exit` unit → `setup` with new model.

---

## Model landscape for AX630C (June 2026)

**Platform tag = `AX620E` / `AX630C` / `-p256`.** Builds tagged only `AX650`/`AX8850` are the *bigger* sibling (3–4× faster) — don't read their tok/s as ours. **Module ceiling ≈ 2B-class INT4** (~3 GB NPU).

### Pre-packaged (apt, ready now)
Qwen2.5-0.5B (default) · Qwen2.5-1.5B · Llama-3.2-1B · DeepSeek-R1-Distill-Qwen-1.5B · InternVL2.5-1B-MPO (VLM) · Whisper tiny/base · MeloTTS · YOLO11.

### Newest worth trying (have real AX630C/AX620E ports)
1. **Qwen3-0.6B** — newest small chat; **run non-thinking mode** (thinking mode tanks latency on 3.2 TOPS).
2. **Qwen3-VL-2B-Instruct (AX630C INT4 build)** — best modern VLM that *fits* the module (~2.0 GB). The headline "show the deck the camera" upgrade.
3. **MiniCPM4-0.5B** — edge-tuned, confirmed **12 tok/s** on AX630C.
4. **SmolLM2-360M** — snappiest, confirmed **14 tok/s**.
5. **InternVL3.5-1B** — lighter VLM (~7 tok/s, ~3 s TTFT) if Qwen3-VL-2B is too heavy.

### Confirmed AX630C throughput (the only published numbers)
| Model | decode | TTFT |
|---|---|---|
| SmolLM2-360M (w8a16) | 14 tok/s | — |
| MiniCPM4-0.5B (w8a16) | 12 tok/s | — |
| HY-MT1.5-1.8B (INT4) | ~13–17 tok/s | 157 ms @ 23 tok |
| InternVL3.5-1B (VLM INT4) | 7.33 tok/s | 3.1 s |

**Load-bearing finding:** TTFT is dominated by **prompt length**, not model size. HY-MT-1.8B: **157 ms @ 23 tokens vs ~11.5 s @ 512-token prefill**. ⇒ For "ask the deck", keep the system prompt + injected engagement context **short** — that matters more than model choice.

### Sweet spots
- **Snappy Q&A:** Qwen2.5-0.5B w4a16 (default) or SmolLM2-360M. Keep prompts short.
- **Best quality that stays usable:** Qwen3-0.6B (non-thinking) or Qwen2.5-1.5B INT4 (~6 tok/s).
- **Camera VLM:** Qwen3-VL-2B INT4 (quality) / InternVL3.5-1B (speed).

### Converting your own model
`pulsar2 llm_build --chip AX620E …` converts stock HF weights → `.axmodel` (GGUF/HF won't run directly). Supported archs: Qwen3/2.5, DeepSeek-R1-Distill, MiniCPM4, InternVL2.5/3, SmolLM2, Llama3.2, Gemma2, Phi2/Phi3, TinyLlama. **Phi-4 not supported.** Qwen3-0.6B converts in ~5 min on a server.

---

## What this means for warlock-buddy

- **Phase 1–2 (mirror + gate)** need only the CoreS3 + BLE + the bridge → Warlock FastAPI. No LLM dependency. Ship first.
- **Phase 3 (ask the deck)** has two viable architectures now:
  - **A) UART:** CoreS3 ↔ Module-LLM over PORT.C. Simple, self-contained, but the CoreS3 is in the middle.
  - **B) Ethernet (via LLM Mate):** Module-LLM on the LAN as an OpenAI-compatible endpoint; can query Warlock directly. More powerful for a grounded "ask the deck," more setup.
- **Grounding:** the LLM only knows engagement state if we inject it into the prompt. Keep that context SHORT (per the TTFT finding).
- **Model pick for v1:** start with the pre-installed **Qwen2.5-0.5B** (zero setup, fastest), leave **Qwen3-0.6B** and **Qwen3-VL-2B** (camera) as upgrades.
- **Bonus capability unlocked:** with ASR+TTS on-module + the CoreS3 mic/speaker, a **fully offline voice** "ask the deck" is feasible (KWS→ASR→LLM→TTS). And the camera + Qwen3-VL-2B = "point the deck at a thing and ask."

---

## Sources
- CoreS3: https://docs.m5stack.com/en/core/CoreS3 · https://shop.m5stack.com/products/m5stack-cores3-esp32s3-iotdevelopment-kit
- DinBase: https://docs.m5stack.com/en/base/DIN%20BASE
- Module13.2 Battery: https://docs.m5stack.com/en/module/battery13.2
- M5GO Bottom3 (IR LED): https://docs.m5stack.com/en/module/M5GO3%20Bottom
- Mini IR Unit: https://docs.m5stack.com/en/unit/ir
- Module-LLM: https://docs.m5stack.com/en/module/Module-llm
- LLM Module Kit (Mate + Ethernet): https://docs.m5stack.com/en/module/Module%20LLM%20Kit · https://shop.m5stack.com/products/m5stack-llm-large-language-model-module-kit-ax630c
- StackFlow API: https://docs.m5stack.com/en/stackflow/module_llm/api · PDF v1.0.0: https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/protocol/M140/LLM_Module_API_v1.0.0_EN.pdf
- Arduino lib: https://github.com/m5stack/M5Module-LLM · StackFlow: https://github.com/m5stack/StackFlow
- OpenAI-API + model list: https://docs.m5stack.com/en/stackflow/openai_api/models · software/apt: https://docs.m5stack.com/en/stackflow/module_llm/software
- AXERA ax-llm: https://github.com/AXERA-TECH/ax-llm · ModelZoo: https://huggingface.co/AXERA-TECH · Pulsar2: https://pulsar2-docs.readthedocs.io/en/latest/appendix/build_llm.html
- M5Stack Ethernet kit coverage: https://linuxgizmos.com/m5stack-expands-offline-llm-lineup-with-ethernet-enabled-kit/
