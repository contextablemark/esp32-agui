# Flashing the ESP32-S3-Touch-AMOLED-1.8

This board is an **ESP32-S3** with USB-C (native USB Serial/JTAG). Connect it with a USB-C
**data** cable and flash directly with `idf.py`. Source the IDF env first
(`. ~/esp/esp-idf/export.sh`) and build (`idf.py build` from `agui-voice/`).

## What actually gets written

`idf.py -p <PORT> flash` writes **three** regions for this app — it does **not** erase the whole
chip:

| Offset | Image | What it is |
|---|---|---|
| `0x0` | `bootloader/bootloader.bin` | 2nd-stage bootloader |
| `0x8000` | `partition_table/partition-table.bin` | partition map (nvs / phy_init / 3 MB app) |
| `0x10000` | `agui_voice.bin` | the application |

The partition map ([../agui-voice/partitions.csv](../agui-voice/partitions.csv)) places **`nvs` at
`0x9000`** (right after the partition table). NVS is where the device stores your WiFi credentials,
Soniox key, AG-UI URL + token, time zone, voice, and volume — so **don't overwrite it**.

## Normal flash + monitor

```bash
cd agui-voice
idf.py -p <PORT> flash monitor      # exit monitor with Ctrl-]
# <PORT>: /dev/ttyACM0 (Linux) · /dev/cu.usbmodem* (macOS) · COMx (Windows)
```

If `Connecting...` won't sync, force ROM download mode: **hold BOOT, tap RESET, release BOOT**,
then retry. Monitor baud is 115200.

> ⚠️ **Flash the app alone at `0x10000`** (`build/agui_voice.bin`) on subsequent reflashes.
> Flashing a **merged** image at `0x0` (`build/agui_voice_merged.bin`) re-pads the `nvs` region
> with `0xFF` and **wipes your saved WiFi/keys** — you'll have to re-provision via the
> `AMOLED-setup` captive portal. App-only reflash keeps NVS intact:
>
> ```bash
> esptool.py --chip esp32s3 -p <PORT> write_flash 0x10000 build/agui_voice.bin
> ```

**Verify the running build** from the boot log's `ELF file SHA256:` line — the compile-time
timestamp is stale on incremental builds, so the ELF hash is the reliable identity check.

## Restoring factory Xiaozhi firmware

The factory images are **full 16 MB flash dumps** in [../Firmware/](../Firmware/) — write at `0x0`
(this restores the entire stock experience, WiFi-setup UI included, and overwrites everything):

- **V1 (SH8601 + FT3168)** → `ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_250805.bin`
- **V2 (CO5300 + CST816)** → `ESP32-S3-Touch-AMOLED-1.8-V2-FactoryXiaozhi_260601.bin`

```bash
esptool.py --chip esp32s3 -p <PORT> write_flash 0x0 \
  Firmware/ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_250805.bin
```

## Quick reference

- Build: `idf.py build` in `agui-voice/`. Show sizes: `/idf-size`.
- Flash helper: `/idf-flash`. Monitor: `/idf-monitor` (baud 115200).
- Download mode: hold BOOT, tap RESET, release BOOT.
- App-only reflash (keeps NVS): `write_flash 0x10000 build/agui_voice.bin`.
