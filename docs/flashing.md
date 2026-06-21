# Flashing the ESP32-S3-Touch-AMOLED-1.8

This board is an **ESP32-S3** with USB-C. The catch in our setup: **this dev container has no
USB**, so `idf.py flash` can't see the board from here. You flash from **your local machine**
instead. Two paths — the browser one is the default here.

> Build artifacts already exist for the LVGL example at
> `examples/ESP-IDF-v5.5.1/05_LVGL_WITH_RAM/build/`. The browser flasher reads
> `build/flasher_args.json`, which already points at the three images below.

## What actually gets written

`idf.py flash` (and the browser flasher) write **three** regions — it does **not** erase the
whole chip:

| Offset | Image | What it is |
|---|---|---|
| `0x0` | `bootloader/bootloader.bin` | 2nd-stage bootloader |
| `0x8000` | `partition_table/partition-table.bin` | partition map (nvs / phy_init / 3 MB app) |
| `0x10000` | `example_qspi_with_ram.bin` | the application (922 KB) |

Flashing this **overwrites the factory Xiaozhi app** → the factory WiFi-setup UI is gone until
you restore it (see bottom). Everything is recoverable from the bundled factory image.

## Path A — Browser WebSerial with a merged image (recommended here)

⚠️ **The `esp-idf-web` VS Code extension only runs in *browser-based* VS Code, not Desktop.**
Its manifest has a `browser` entry and no Node `main`, so VS Code Desktop never loads it (you'll
see "ESP-IDF" but not "ESP-IDF-Web"). So we skip the extension and use a browser flasher page.

To make this one-click, build a **single merged image** flashed at `0x0`:
```bash
source /opt/esp/idf/export.sh
cd examples/ESP-IDF-v5.5.1/05_LVGL_WITH_RAM/build
esptool.py --chip esp32s3 merge_bin -o merged.bin \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 example_qspi_with_ram.bin
```
Then on **your computer** (no container USB needed):
1. Get `merged.bin` onto your machine (download from VS Code, or have Claude send it).
2. **Plug the board in** with a USB-C **data** cable; use **Chrome/Edge** (WebSerial isn't in
   Safari/Firefox).
3. Open **ESP Launchpad** → https://espressif.github.io/esp-launchpad/ → **"DIY"** tab.
4. Chip **ESP32-S3**, add `merged.bin` at address **`0x0`**, **Connect** → pick the port → **Flash**.
5. If Connect won't sync: **hold BOOT, tap RESET, release BOOT**, retry.
6. **Reset** the board after flashing.

**Expected result for this example:** the AMOLED shows the LVGL demo and touch responds — that
confirms your flash round-trip. (Audio/WiFi aren't exercised by this example — those come with our
`net_prov`/`soniox_client` work.)

### Path A2 — local esptool (most reliable, no browser)
```bash
pip install esptool
esptool --chip esp32s3 -p <PORT> write_flash 0x0 merged.bin
# <PORT>: /dev/ttyACM0 (Linux) · /dev/cu.usbmodem* (macOS) · COM5 (Windows)
```

### Using the esp-idf-web extension anyway
Open this workspace in **browser-based** VS Code (the web editor). There the web extension host
exists, the extension activates (on `sdkconfig`/`CMakeLists.txt`), and `ESP-IDF-Web Flash` /
`ESP-IDF-Web Monitor` appear in the palette. It reads `build/flasher_args.json` directly.

## Path B — USB passthrough to the container

The container runs `--privileged`, so if you can attach the host's USB device to it, the normal
flow works:

```bash
ls /dev/ttyACM* /dev/ttyUSB*          # find the port after passthrough
source /opt/esp/idf/export.sh
cd examples/ESP-IDF-v5.5.1/05_LVGL_WITH_RAM
idf.py -p /dev/ttyACM0 flash monitor   # exit monitor with Ctrl-]
```

On Docker Desktop (macOS/Windows) USB passthrough is awkward; Path A is usually easier. On native
Linux you can pass `--device=/dev/ttyACM0` to the container.

## Restoring factory Xiaozhi firmware

The factory images are **full 16 MB flash dumps** in [../Firmware/](../Firmware/) — write at `0x0`:

- **This board (V1: SH8601 + FT3168)** → `ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_250805.bin`
- V2 (CO5300 + CST816) → `ESP32-S3-Touch-AMOLED-1.8-V2-FactoryXiaozhi_260601.bin`

```bash
esptool.py --chip esp32s3 write_flash 0x0 \
  Firmware/ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_250805.bin
```

(or flash that single `.bin` at `0x0` via esp-idf-web). This brings back the entire stock
experience, WiFi-setup UI included.

## Quick reference

- Build first (already done): `idf.py build` in the example dir.
- Show sizes: `/idf-size`. Flash helper: `/idf-flash`. Monitor: `/idf-monitor`.
- Baud for monitor: 115200. Download mode: hold BOOT, tap RESET, release BOOT.
- A good no-risk first test: flash the example, confirm the display, then (optionally) restore the
  factory `.bin` — proving both directions of the workflow before we iterate on custom firmware.
