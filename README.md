# ESP32-S3 On-Device AG-UI Voice Client

Custom **ESP-IDF 5.5.x** firmware that turns a **Waveshare ESP32-S3-Touch-AMOLED-1.8** into a
*first-class [AG-UI](https://docs.ag-ui.com) client* — not a thin voice satellite.

The device captures mic audio and streams it to **[Soniox](https://soniox.com)** for real-time
speech-to-text, is itself the **AG-UI client** (it POSTs `RunAgentInput` and consumes the SSE
event stream directly on-device), and renders the conversation plus live agent activity on the
1.8″ AMOLED via **LVGL 8.4**. Because the AG-UI client lives on the device, agent events
(`TOOL_CALL_*`, reasoning, interrupts) drive the screen, and the board can expose its own
sensors / screen / touch back to the agent as tools and ambient context.

> **Status:** v1 in progress — P0–P4 implemented (WiFi + provisioning, Soniox streaming STT,
> AG-UI text client, LVGL chat UI, live agent-status line). P5–P8 (ambient context, HITL
> interrupts, client tools, hardening) are planned. See [Roadmap](#roadmap).

```
mic ─ES8311/I²S(16k s16le)─▶ Soniox WSS (streaming) ─▶ live transcript
   ─▶ AG-UI: POST RunAgentInput{messages, context} ─▶ SSE event stream
   ─▶ LVGL: chat bubbles (TEXT_*) · ephemeral status (REASONING_*/TOOL_CALL_*/RUN_*)
   ─▶ (planned) client tools run on-device · interrupt prompts answered by touch
```

---

## Hardware

**Board:** [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8)

| | |
|---|---|
| **MCU** | ESP32-S3 (Xtensa LX7 dual-core, 240 MHz), 16 MB flash, 8 MB octal PSRAM @ 80 MHz |
| **Display** | 1.8″ AMOLED, **368 × 448 px**, QSPI |
| **Audio** | ES8311 codec + dual digital mics (16 kHz mono capture) |
| **Sensors** | QMI8658 6-axis IMU · AXP2101 PMIC · PCF85063 RTC |
| **Input** | Capacitive touch · top "BOOT" button (GPIO0) repurposed as push-to-talk |
| **Other** | SD/MMC slot · 3.7 V Li battery (MX1.25) · USB-C (native USB Serial/JTAG) |

Two hardware revisions are auto-detected at runtime by probing the touch controller over I²C —
**V1** (SH8601 display + FT3168 touch @ 0x38) and **V2** (CO5300 + CST816 @ 0x15). One binary
serves both. Full pinout is in [CLAUDE.md](CLAUDE.md).

---

## Architecture

```
        ┌──────────────────── ESP32-S3 (ESP-IDF, LVGL) ─────────────────────┐
 mic ─ES8311─I²S─▶ audio task ─ring─▶ soniox_client ════ wss ════▶ Soniox STT
                                            │ transcript (partial + is_final)
 QMI8658/AXP2101/PCF85063 ─▶ device_tools.context ─┐
                                                   ▼
                          agui_client ═══ https POST RunAgentInput ═══▶ AG-UI agent
                                      ◀═════════════ SSE events ════════
                                            │  (event router)
                  ┌─────────────────────────┼───────────────────────────┐
                  ▼            ▼             ▼              ▼             ▼
              chat bubbles  ephemeral    interrupt      client tools   idle
              (TEXT_*)      status       prompt (HITL)  (timer/qr)     timer
        └────────────────────────────── AMOLED ─────────────────────────────┘
```

Secrets live in NVS. TLS sessions run **sequentially** (Soniox closes on end-of-speech before
the AG-UI run opens), so peak load is one mbedTLS session; large buffers are kept in PSRAM.
No barge-in (single ES8311, no echo-reference channel) → push-to-talk turn-taking.

The on-device AG-UI client is a from-scratch ESP-IDF port *inspired by* the community
**C++ AG-UI SDK** (`ag-ui-protocol/ag-ui` → `sdks/community/c++`): libcurl → `esp_http_client`
(streaming), nlohmann/json → cJSON, exceptions/RTTI dropped, buffers in PSRAM, plus three
device extensions (per-run ambient `context`, interrupt → resume, client-tool dispatch).

---

## Repository layout

```
esp32-agui/
├── agui-voice/                  ← the ESP-IDF project (self-contained; build this)
│   ├── main/                    ← app entry + PTT turn state machine
│   ├── components/              ← first-party components (below) + vendored board BSP/drivers
│   ├── sdkconfig.defaults       ← target esp32s3, TLS 1.3, PSRAM, LVGL config
│   └── partitions.csv
├── docs/
│   ├── agui-voice-plan.md       ← full design: components, APIs, state machine, build phases
│   ├── flashing.md              ← flash workflow (cloud container has no USB)
│   └── soniox-rt-protocol.md    ← verified Soniox real-time WSS protocol notes
├── .devcontainer/               ← pinned to espressif/idf:release-v5.5
├── .claude/commands/            ← /idf-build /idf-flash /idf-monitor /idf-qemu /idf-size /idf-docs
├── CLAUDE.md                    ← project guide (hardware, toolchain, layout, conventions)
└── README.md                    ← you are here
```

### First-party firmware components ([agui-voice/components/](agui-voice/components/))

| Component | Responsibility |
|---|---|
| [`agui_client`](agui-voice/components/agui_client/) | AG-UI client: POST `RunAgentInput` → streaming SSE parser → event router → handler callbacks |
| [`soniox_client`](agui-voice/components/soniox_client/) | ES8311 mic capture → Soniox WSS streaming STT → partial/final transcript callbacks |
| [`chat_ui`](agui-voice/components/chat_ui/) | LVGL chat bubbles, ephemeral status line, (planned) interrupt prompt + QR |
| [`net_prov`](agui-voice/components/net_prov/) | Multi-SSID WiFi connect + auto-reconnect + SoftAP captive-portal provisioning |
| [`app_cfg`](agui-voice/components/app_cfg/) | Tiny NVS-backed config/secret store (Soniox key, AG-UI URL + bearer) |
| [`device_tools`](agui-voice/components/device_tools/) | Ambient context (motion/battery/time) + client-tool registry (timer/alarm/QR) |
| [`esp32_s3_touch_amoled_1_8`](agui-voice/components/esp32_s3_touch_amoled_1_8/) | Board BSP (display/touch/audio bring-up); vendored from the Waveshare LVGL example |
| `board_variant`, `esp_lcd_*`, `espressif__*` | Vendored display/touch drivers + ESP component-registry deps |

---

## Toolchain — ESP-IDF 5.5.x **only**

The managed components cap at `idf <6.0` (e.g. `espressif/es8311` requires `>=4.4,<6.0`), so
**ESP-IDF 6.x breaks dependency solving**. If you see
`ERROR: Version solving failed: no versions of idf match >=5.5.0,<6.0.0`, you're on the wrong IDF.

The devcontainer ([.devcontainer/Dockerfile](.devcontainer/Dockerfile)) is pinned to
`espressif/idf:release-v5.5`. Open the repo in VS Code → **Dev Containers: Reopen in Container**.
Target chip is always **esp32s3** (comes from `sdkconfig.defaults` — don't run `idf.py set-target`,
it wipes `sdkconfig`).

---

## Build, flash, run

All commands run from inside the project dir. The devcontainer's `.bashrc` already sources the
IDF env (`source /opt/esp/idf/export.sh`).

```bash
cd agui-voice
idf.py build
```

### Flashing

This is a **cloud devcontainer with no USB passthrough**, so flash one of two ways:

1. **Browser (WebSerial):** the [`esp-idf-web`](https://marketplace.visualstudio.com/items?itemName=Espressif.esp-idf-web)
   VS Code extension is pre-installed — flash directly from the browser over WebSerial.
2. **Local USB:** pass the board through to the container (it runs `--privileged`), then
   `idf.py -p <PORT> flash monitor`.

> ⚠️ **Flash the app alone at `0x10000`** (`build/agui_voice.bin`). Flashing a *merged* image at
> `0x0` pads the NVS region (0x9000) with `0xFF` and **wipes your saved WiFi/keys**. See
> [docs/flashing.md](docs/flashing.md) for the recovery procedure and download-mode entry.

**Verify the running build** from the boot log's `ELF file SHA256:` line — the compile-time
timestamp is stale on incremental builds, so the ELF hash is the reliable identity check.

### Monitor

Console is routed to the chip's native **USB Serial/JTAG** (the USB-C port, VID `0x303a`/PID
`0x1001`), so `idf.py monitor` shows `ESP_LOG` output over USB-C.

### QEMU

`idf.py qemu` emulates **CPU / RAM / UART only** — the AMOLED, touch, I²C sensors, SD, and codec
are not emulated. Useful for boot / app-logic / networking, not the UI.

---

## First-run setup & usage

1. **Provision.** With no saved credentials, the device starts a SoftAP captive portal named
   **`AMOLED-setup`**. Join it from a phone, and the form lets you enter:
   - WiFi SSID + password
   - **Soniox API key**
   - **AG-UI endpoint URL** (+ optional bearer token)

   Submitted values are saved to NVS; the device connects and is ready.

2. **Talk.** **Hold the top button** (the "BOOT" button) to talk — the Soniox session opens only
   while held, with a live transcript scrolling across the top status line. **Release** to send the
   utterance to your AG-UI agent; the reply streams into a chat bubble while the status line shows
   what the agent is doing (`Thinking… → Reasoning… → Using web_search… →` reply).

3. **Reconfigure without reflashing.** **Double-tap** the top button to reopen the setup portal and
   change the endpoint / keys (only fields you fill in are overwritten).

Configuration and secrets are stored in NVS (`appcfg` namespace for the Soniox key / AG-UI URL +
token; `netprov` for WiFi). Soniox **ephemeral keys** (mint-on-demand) are planned for P8.

---

## Roadmap

| Phase | Goal | Status |
|---|---|---|
| **P0** Environment + WiFi | IDF 5.5 pinned; `net_prov` multi-SSID connect + auto-reconnect | ✅ |
| **P0.5** Provisioning | SoftAP captive portal (`AMOLED-setup`) + web form | ✅ |
| **P1** Soniox STT | `soniox_client`: ES8311 capture → WSS → transcript | ✅ |
| **P2** AG-UI text | `agui_client` core (RunAgentInput + SSE + `TEXT_*`) | ✅ |
| **P3** Chat UI | `chat_ui` chat list + streaming + BOOT-button PTT | ✅ |
| **P4** Status | `REASONING_*`/`TOOL_CALL_*`/`RUN_*` → live status line | ✅ |
| **P5** Context | ambient motion + battery + time → per-run `context` | ◻ planned |
| **P6** Interrupt | detect HITL → render-by-`response_schema` → resume; `show_qr` | ◻ planned |
| **P7** Tools | `set_timer` (idle countdown) + `set_alarm` (RTC) round-trip | ◻ planned |
| **P8** Hardening | Soniox ephemeral keys, reconnect/backoff, PSRAM tuning, error states | ◻ planned |

Backlog (post-P8): A2UI generative-UI rendering, wake-word turn control, Soniox accuracy tuning,
v2 tools (`display_card`/`render_chart`/`ble_*`/`sd_*`), `STATE_*` + JSON-Patch shared state.

The full design — component APIs, runtime state machine, AG-UI event→UI mapping, and per-phase
acceptance criteria — is in [docs/agui-voice-plan.md](docs/agui-voice-plan.md).

---

## Credits & licenses

This project is licensed under the [MIT License](LICENSE).

It builds on, ports, or interoperates with:

- **AG-UI protocol** ([ag-ui-protocol/ag-ui](https://github.com/ag-ui-protocol/ag-ui)) — the
  on-device client is a from-scratch ESP-IDF port inspired by the community **C++ SDK**
  (`sdks/community/c++`); no SDK code is vendored here.
- **Soniox** real-time STT (WSS, model `stt-rt-v5`) — protocol notes in
  [docs/soniox-rt-protocol.md](docs/soniox-rt-protocol.md).
- **Waveshare ESP32-S3-Touch-AMOLED-1.8 BSP + drivers**
  ([waveshareteam/ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8))
  — vendored under `agui-voice/components/`.
- **Espressif ESP-IDF**, **LVGL 8.4**, and ESP component-registry packages
  (`esp_lcd_sh8601`/`esp_lcd_co5300`, `esp_lcd_touch_*`, `button`, `esp_codec_dev`,
  `esp_websocket_client`, …).

Driver/API questions can be researched against the upstream repos via the DeepWiki MCP
(`/idf-docs <question>`) — see [CLAUDE.md](CLAUDE.md) for the configured sources.
