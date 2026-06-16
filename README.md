# ESPHome — LilyGo T5 E-Paper S3 Pro

ESPHome custom component for the [LilyGo T5 4.7" E-Paper S3 Pro](https://www.lilygo.cc/)
(ESP32-S3, ED047TC1 4.7" 960×540 e-paper, GT911 touch, TPS65185 PMIC,
PCA9535 GPIO expander, BQ27220 fuel gauge, BQ25896 charger).

This is a fork of [nickolay/esphome-lilygo-t547plus](https://github.com/nickolay/esphome-lilygo-t547plus)
rewritten for the S3 Pro hardware. The original supported the
T5 4.7" v2.3 (ESP32, not S3) under a different component name.

> **Not for the original T5 4.7"** — different MCU, different I2C/GPIO map,
> different power sequencing. Use the upstream repo if you have that board.

## Build

```sh
git clone https://github.com/nickolay/esphome-lilygo-t547plus.git
cd esphome-lilygo-t547plus
esphome run mod.yaml
```

Requires:
- ESPHome ≥ 2023.12 (tested on 2026.4.5)
- PlatformIO with the `pioarduino` platform (ESP-IDF 5.5.x)

`mod.yaml` is a working example that drives a Home Assistant remote-control
UI (rooms, lights, climate, media). Adapt the `display:` lambda for your own
use case.

## Component

```yaml
external_components:
  - source:
      type: local
      path: components/
    components: ["t5s3_epaper"]

display:
  - platform: t5s3_epaper
    id: t5_display
    rotation: 270
    update_interval: 60s
    lambda: |-
      it.fill(Color(255, 255, 255));
      # ... your draw code ...

touchscreen:
  - platform: t5s3_epaper
    id: t5_touch
    display: t5_display
    update_interval: 50ms
    on_touch:
      - lambda: |-
          ESP_LOGI("touch", "x=%d y=%d", touch.x, touch.y);
```

## Notes

- Uses [Frogy76/epdiy](https://github.com/Frogy76/epdiy) (ESP-IDF 5.5.x compatible).
- Board init uses the **legacy** I2C driver (`driver/i2c.h`); the new
  `i2c_master` driver in ESP-IDF 5.5.4 has an `ESP_ERR_INVALID_STATE` bug.
- E-paper refresh runs on a FreeRTOS task pinned to core 0 so the main loop
  and HA API stay responsive during the ~100 ms DU / ~1.5 s GL16 waveforms.
