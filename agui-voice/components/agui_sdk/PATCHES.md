# agui_sdk — vendored AG-UI C++ SDK + device patches

This component vendors the upstream **AG-UI community C++ SDK** and applies a small set of
device-specific changes. The goal is to track upstream closely (easy re-sync) and keep the deltas
PR-ready for upstream.

## Upstream source

- Repo: `ag-ui-protocol/ag-ui`
- Path: `sdks/community/c++/src/` (+ `LICENSE`, MIT)
- Pinned commit: **`4202bf237179b68e74cec8454bdf884e83db7e41`**

Re-sync: copy `sdks/community/c++/src` over `agui_sdk/src` at a newer commit, then re-apply the
deltas below (all are marked with a `// [device]` comment in-tree, so `grep -rn "\[device\]" src`
lists them).

**After re-syncing, run the host tests:** `test/run_host_tests.sh` (plain g++, no ESP-IDF/hardware)
round-trips the REASONING_* / interrupt / resume extensions and fails loudly if a delta was dropped
or upstream changed a field/shape. Run it before trusting a re-synced snapshot.

## Deltas from upstream

### A. Transport: libcurl removed (ESP-IDF uses esp_http_client)
- **Removed** `src/http/http_service.cpp` (the libcurl `HttpService`). Its interface header
  `src/http/http_service.h` is kept — the device implements `IHttpService` as `EspHttpService` in
  the **agui_client** component (`esp_http_service.{h,cpp}`) over `esp_http_client`.
- `src/agent/http_agent.cpp` constructor: the default `m_httpService = std::make_unique<HttpService>()`
  is replaced with `m_httpService = nullptr` (so libcurl is never referenced); `runAgent()` gains a
  null-guard that errors cleanly if no transport was injected. The caller always calls
  `setHttpService(EspHttpService)` after `build()`.

### B. REASONING_* events (current protocol; first-class)
Upstream only models the **deprecated** `THINKING_*` family. Added the current protocol's
`REASONING_*` events (mirrors `sdks/typescript` core `events.ts`) as first-class events:
- `src/core/event.h` — `EventType` values `ReasoningStart, ReasoningMessageStart,
  ReasoningMessageContent, ReasoningMessageEnd, ReasoningMessageChunk, ReasoningEnd,
  ReasoningEncryptedValue` + the matching event structs.
- `src/core/event.cpp` — `parseEventType()` strings, `eventTypeToString()`, the `parse()` factory
  cases, and `toJson()`/`fromJson()` for each.
- `src/core/subscriber.h` / `.cpp` — `IAgentSubscriber` `onReasoning*` virtuals, `EventHandler`
  dispatch in both switches, and a `m_reasoningBuffer` (ephemeral, like thinking).

### C. Interrupt outcome + resume (HITL; first-class)
Upstream's `RunFinishedEvent` carried only a generic `result`. Added the protocol's interrupt model:
- `src/core/session_types.h` / `.cpp` — `Interrupt`, `ResumeStatus`, `ResumeEntry` types; a
  `resume[]` field on `RunAgentInput` (emitted/parsed in `toJson`/`fromJson`) and `RunAgentParams`
  (+ `addResume()`).
- `src/core/event.h` / `.cpp` — `RunFinishedEvent` gains `outcomeType` + `interrupts[]` (+
  `isInterrupt()`), parsed from / emitted to the `outcome` discriminated union.
- `src/agent/http_agent.cpp` — `runAgent()` copies `params.resume` into `RunAgentInput.resume`.

> Note: the on-device interrupt **UI** (render-by-`responseSchema`, resume run) is firmware-side
> (P6) and not part of this component.

## Not changed
Unknown event types are intentionally left to upstream's behavior: `parseSseEventData()` already
catches the `parseEventType()` throw and **skips** unknown types with a warning (forward-compatible),
so no "lenient parser" change is needed.

## Upstreaming
Deltas **B** and **C** are written to be contributable back to `ag-ui-protocol/ag-ui`. Delta **A**
is ESP-IDF-specific (transport swap) and stays local. Opening the upstream PR is future work.
