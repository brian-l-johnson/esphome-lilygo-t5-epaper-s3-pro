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

 protected:
  void update_touches() override;

 private:
  bool gt911_read_(uint16_t reg, uint8_t *buf, size_t len);
  bool gt911_write_(uint16_t reg, uint8_t val);

  bool setup_done_{false};
  uint8_t gt911_addr_{0x5D};  // may switch to 0x14 depending on INT state at reset
};

}  // namespace t5s3_epaper
}  // namespace esphome

#endif  // USE_ESP32
