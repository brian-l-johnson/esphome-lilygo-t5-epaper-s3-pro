# ESPHome — LilyGo T5 E-Paper S3 Pro

ESPHome custom component for the [LilyGo T5 4.7" E-Paper S3 Pro](https://www.lilygo.cc/):

- **MCU**: ESP32-S3 with 16 MB flash, 8 MB OCT PSRAM
- **Display**: ED047TC1, 4.7", 960×540, 16-level grayscale, parallel bus
- **Touch**: GT911 capacitive
- **PMIC**: TPS65185
- **GPIO expander**: PCA9535
- **Battery**: BQ27220 fuel gauge + BQ25896 charger

> This is a fork of [nickolay/esphome-lilygo-t547plus](https://github.com/nickolay/esphome-lilygo-t547plus)
> rewritten for the S3 Pro hardware. **Not for the original T5 4.7" v2.3**
> (different MCU, different I2C/GPIO map). Use the upstream repo if you have
> that board.

## Files in this repo

| File | What it is |
|---|---|
| `example.yaml` | **Start here.** Minimal config: clock + battery + touch logging. ~110 lines. |
| `office-remote.yaml` | The author's working config: 3-room HA remote control (lights, climate, media). ~1000 lines. Useful as a reference but won't run as-is for you — references specific HA entity IDs. |
| `components/t5s3_epaper/` | The ESPHome external component (display + touchscreen). |
| `secrets.yaml.template` | Copy to `secrets.yaml` and fill in. |
| `wifi-secrets.yaml.template` | Copy to `wifi-secrets.yaml` and fill in. |

## Quick start

```sh
git clone https://github.com/nickolay/esphome-lilygo-t547plus.git
cd esphome-lilygo-t547plus

cp secrets.yaml.template     secrets.yaml      # fill in: api_encryption_key, api_ota_password
cp wifi-secrets.yaml.template wifi-secrets.yaml # fill in: WiFi SSID + password

esphome run example.yaml
```

Requires:
- ESPHome ≥ 2023.12 (tested on 2026.4.5)
- PlatformIO with the `pioarduino` platform (ESP-IDF 5.5.x)

After flashing you should see a clock + battery readout on the e-paper. If
the display is blank or refreshes are very slow, check the **Gotchas**
section below.

## Touch calibration

**You must calibrate touch for your unit.** Per-panel manufacturing variance
shifts the GT911's raw coordinate range by ~30 px. Without calibration,
taps land in the wrong place.

Procedure:

1. **Enable temporary verbose touch logging.** Edit `components/t5s3_epaper/t5s3_touch.cpp`,
   find the `ESP_LOGV` near the end of `update_touches()`, and change `ESP_LOGV` →
   `ESP_LOGI`. Reflash.

2. **Tap each of the 4 corners** as close to the actual corner as you can,
   one at a time. Watch `esphome logs example.yaml`. You'll see lines like:

   ```
   [touch:178]: Touch 0: id=0 raw(12,15) screen(0,0)
   ```

   Record the **raw** values from each corner — average a few taps per corner.

3. **Compute the constants.** From the 4 sets of (raw_x, raw_y):

   ```
   x_min = average of raw_x at top-left and bottom-left
   x_max = average of raw_x at top-right and bottom-right
   y_min = average of raw_y at top-left and top-right
   y_max = average of raw_y at bottom-left and bottom-right
   ```

4. **Update the `calibration:` block** in your YAML:

   ```yaml
   touchscreen:
     - platform: t5s3_epaper
       calibration:
         x_min: 12
         x_max: 533
         y_min: 15
         y_max: 945
   ```

5. **Revert the log change** (`ESP_LOGI` → `ESP_LOGV`) and reflash.

The defaults baked into `example.yaml` and the component header are from the
author's unit and *probably* close enough to get you started, but recalibrate
for production use.

## Component API

The component exposes some non-standard methods you can call from your
display lambda or touch `on_touch` handlers via `id(t5_display).method_name()`:

| Method | What it does |
|---|---|
| `fill(color)` | Override of `Display::fill` — `memset`s the framebuffer instead of pixel-by-pixel. Already used by `it.fill()`. |
| `fast_rect(x, y, w, h, color)` | Solid-color filled rectangle via row-wise `memset`. Use for big rects (>~50 K px) instead of `it.filled_rectangle()` — `filled_rectangle` isn't virtual so we can't override it, but `fast_rect` is ~20× faster. Coords are in the lambda's logical (rotated) space. |
| `request_full_refresh()` | Force the next refresh to GL16 (clears DU ghosting). Call before `update()`. |
| `request_refresh()` | Coalesced refresh request — collects bursts of HA state changes into a single render after a 150 ms settling window. Prefer this over `update()` for HA `on_state` handlers. |
| `update()` | Immediate render. Use for touch — instant feedback. |
| `notify_touch()` | Touch handler should call this on every tap. Lets the component defer auto-GL16 promotion while you're interacting. |
| `set_optimistic(slot, state)` | Optimistic UI: predict the post-action state in slot N (0-7). The render lambda reads via `get_optimistic`; HA's `on_state` clears via `clear_optimistic`. See `office-remote.yaml` for the toggle pattern. |
| `get_optimistic(slot)` | Returns -1 if no override (or expired after 5 s), else 0/1. |
| `clear_optimistic(slot)` | Call from HA `on_state` handlers when HA confirms. |
| `get_battery_pct() / get_battery_mv() / get_battery_ma()` | Cached BQ27220 readings (10 s throttle). Safe to call from the render lambda. |

## Gotchas

**Sdkconfig name typos fail silently.** ESPHome doesn't validate
`sdkconfig_options` keys — a misspelling just gets ignored and the IDF
default applies. Always `grep` the generated
`.esphome/build/<name>/.pioenvs/<name>/config/sdkconfig.h` to confirm what
actually applied. The big one to verify:

```sh
grep DATA_CACHE_LINE .esphome/build/lilygo/.pioenvs/lilygo/config/sdkconfig.h
# Should show: CONFIG_ESP32S3_DATA_CACHE_LINE_64B 1 and CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE 64
```

If you see `_32B 1` instead, refreshes will be ~4× slower (GL16 ≈ 7 s instead
of 1.8 s) because epdiy's `check_cache_configuration()` halves the pixel
clock twice. Set both options:

```yaml
CONFIG_ESP32S3_DATA_CACHE_LINE_32B: n
CONFIG_ESP32S3_DATA_CACHE_LINE_64B: "y"
```

For Kconfig choice groups (only one of N can be active) you need to
explicitly disable the default with `: n`.

**Lambda render slowness on rotated displays.** `it.filled_rectangle()` and
`it.horizontal_line()` aren't virtual, so the component can't override them.
With `rotation: 270` the pixel-loop becomes a column-traversal in physical
memory that thrashes the PSRAM cache. For big rects, prefer
`id(t5_display).fast_rect(x, y, w, h, color)`.

**I2C bus contention with touch polling.** GT911 polls every 50 ms by
default; during a refresh, the flush task uses the same I2C bus for
TPS65185 power sequencing. This is fine (legacy driver serializes), but
heavy DEBUG-level logging over USB-CDC can back-pressure the main thread
and add seconds to renders. Use `logger.level: INFO` in production.

**Hardware diagnostics.** `s3_board_init()` has ~120 lines of HW-failure
diagnostics (USB_SERIAL_JTAG / BRIDGE_EN test, 5 s VDD0V3 wait loop) for
debugging dead VDD0V3 rails. They're gated behind `T5S3_HW_DIAG` — disabled
by default. If your hardware just doesn't work (no I2C devices found), add
to `build_flags`:

```yaml
build_flags:
  - "-DT5S3_HW_DIAG"
```

The boot will be ~6 s slower but you'll get verbose probing output that
helps localize a dead LDO or stuck I2C line.

**PlatformIO esp_adc include path.** The build needs the IDF's `esp_adc`
headers (epdiy includes them, even though they're GC'd at link). The
example uses `${sysenv.HOME}` — adjust if your PlatformIO is elsewhere.
On Windows, replace with `${sysenv.USERPROFILE}` or hardcode the path.

## Architecture notes

- **Board init uses the legacy I2C driver** (`driver/i2c.h`), not the new
  `i2c_master`. ESP-IDF 5.5.4's new driver has an `ESP_ERR_INVALID_STATE`
  bug on TPS65185. The board definition is inlined into `t5s3_epaper.cpp`
  rather than living in epdiy.
- **Frogy76's epdiy fork** is used (vendored via `cg.add_library` in
  `display.py`) — it has fixes for ESP-IDF 5.5.x that aren't merged
  upstream yet.
- **Refresh runs on a FreeRTOS task** pinned to core 0 so the ~1.8 s GL16
  doesn't block the ESPHome main loop on core 1.
- **DU triangle artifact workaround**: the DU waveform's common-electrode
  swings affect all pixels globally. For solid-black areas (e.g., a black
  top bar), unchanged B→B pixels get no per-pixel drive and drift white.
  The flush task pre-conditions the top 84-px-wide strip of `back_fb` to
  white before every DU update, which forces epdiy to compute W→target for
  every top-bar pixel. See the comment in `flush_task_()`.

## License

MIT (inherited from upstream).
