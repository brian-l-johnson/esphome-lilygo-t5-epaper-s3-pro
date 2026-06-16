#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/touchscreen/touchscreen.h"

#ifdef USE_ESP32

namespace esphome {
namespace t5s3_epaper {

class T5S3Touch : public touchscreen::Touchscreen {
 public:
  void setup() override;
  void dump_config() override;
  // Must run after the display component (PROCESSOR=400) which installs I2C.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_interrupt_pin(GPIOPin *pin) { /* not used — polling mode */ }
  void set_reset_pin(GPIOPin *pin)     { /* not used — reset handled by board init */ }

  // Calibration setters — defaults are for unit serial #2 (June 2026); each
  // physical unit's GT911 reports a slightly different raw range, so expose
  // these via YAML for easy re-calibration without a recompile.
  void set_cal_raw_x_min(int v) { cal_raw_x_min_ = v; }
  void set_cal_raw_x_max(int v) { cal_raw_x_max_ = v; }
  void set_cal_raw_y_min(int v) { cal_raw_y_min_ = v; }
  void set_cal_raw_y_max(int v) { cal_raw_y_max_ = v; }

 protected:
  void update_touches() override;

 private:
  bool gt911_read_(uint16_t reg, uint8_t *buf, size_t len);
  bool gt911_write_(uint16_t reg, uint8_t val);

  bool setup_done_{false};
  uint8_t gt911_addr_{0x5D};  // may switch to 0x14 depending on INT state at reset

  // Defaults from 4-corner calibration on unit #2.
  int cal_raw_x_min_{30};
  int cal_raw_x_max_{497};
  int cal_raw_y_min_{32};
  int cal_raw_y_max_{922};
};

}  // namespace t5s3_epaper
}  // namespace esphome

#endif  // USE_ESP32
