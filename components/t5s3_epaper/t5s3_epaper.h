#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/display/display_buffer.h"

#include "epdiy.h"
#include "epd_highlevel.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#ifdef USE_ESP32

namespace esphome {
namespace t5s3_epaper {

class T5S3Epaper : public display::DisplayBuffer {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void display();
  void fill(Color color) override;
  void clear() override { this->fill(Color(255, 255, 255)); }
  void draw_pixel_at(int x, int y, Color color) override;
  // Fast solid-color filled rect: bypasses ESPHome's per-pixel
  // filled_rectangle (which isn't virtual) by writing rows of the physical
  // framebuffer with memset. Coords are in the lambda's logical (rotated)
  // space — same as it.filled_rectangle.
  void fast_rect(int x, int y, int w, int h, Color color);

  void dump_config() override;
  float get_setup_priority() const override;

  float get_battery_pct()  { this->refresh_battery_(); return cached_battery_pct_; }
  float get_battery_mv()   { this->refresh_battery_(); return cached_battery_mv_; }
  float get_battery_ma()   { this->refresh_battery_(); return cached_battery_ma_; }

  // BQ25896 charger status. The fuel gauge's current reading goes to ~0
  // when the battery is topped off, so it can't tell you "USB is plugged
  // in but no current is flowing". The charger can.
  //
  // get_charger_vbus() returns: 0=no input, 1=USB SDP, 2=adapter, 7=OTG
  // get_charger_state() returns: 0=not charging, 1=pre-charge, 2=fast,
  //                              3=charge termination done
  uint8_t get_charger_vbus()  { this->refresh_battery_(); return (cached_charger_status_ >> 5) & 0x07; }
  uint8_t get_charger_state() { this->refresh_battery_(); return (cached_charger_status_ >> 3) & 0x03; }
  bool charger_present()      { return this->get_charger_vbus() != 0 && this->get_charger_vbus() != 7; }
  bool charger_active()       { uint8_t s = this->get_charger_state(); return s == 1 || s == 2; }

  void request_full_refresh() { full_refresh_ = true; }
  // Coalesced refresh request — collects bursts of HA state changes into a
  // single re-render after a short settling window. Use this for HA state
  // events; use update() directly for touch where instant feedback matters.
  void request_refresh() { dirty_ = true; dirty_at_ms_ = millis(); }
  // Touch handler signals here on every tap. Lets display() defer the
  // auto full-refresh promotion if a touch is recent — a 1.8 s GL16
  // freeze mid-interaction is jarring.
  void notify_touch() { last_touch_at_ms_ = millis(); }

  // Optimistic UI cache. Touch handler sets the predicted post-action
  // state; the render lambda reads it via get_optimistic() and uses it
  // until HA confirms (call clear_optimistic in on_state) or 5 s elapse.
  // 8 slots — caller picks the index by convention.
  void set_optimistic(uint8_t slot, bool state) {
    if (slot >= 8) return;
    opt_[slot].state = state ? 1 : 0;
    opt_[slot].ms    = millis();
  }
  void clear_optimistic(uint8_t slot) {
    if (slot >= 8) return;
    opt_[slot].state = -1;
  }
  // Returns -1 if no override or it's expired; otherwise 0/1.
  int get_optimistic(uint8_t slot) {
    if (slot >= 8) return -1;
    if (opt_[slot].state < 0) return -1;
    if ((millis() - opt_[slot].ms) >= OPTIMISTIC_EXPIRE_MS) return -1;
    return opt_[slot].state;
  }

  display::DisplayType get_display_type() override {
    return display::DisplayType::DISPLAY_TYPE_GRAYSCALE;
  }

 protected:
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  int get_width_internal() override  { return 960; }
  int get_height_internal() override { return 540; }

  size_t get_buffer_length_() { return 0; }

  static void flush_task_(void *arg);

  float bq27220_read_reg(uint8_t reg, bool is_signed);
  void refresh_battery_();

  uint8_t *framebuffer_{nullptr};
  EpdiyHighlevelState hl_state_{};
  bool initialized_{false};
  bool full_refresh_{true};      // set before xSemaphoreGive; read by flush task
  bool pending_full_refresh_{false};  // latched copy read inside flush task
  bool needs_update_{false};     // set in update() when flush is still in flight; retried in loop()
  bool dirty_{false};            // set by request_refresh(); coalesced in loop()
  uint32_t dirty_at_ms_{0};      // timestamp of most recent request_refresh()
  uint8_t fast_refresh_count_{0};
  uint32_t last_touch_at_ms_{0};
  static constexpr uint8_t FULL_REFRESH_EVERY = 20;
  static constexpr uint32_t REFRESH_COALESCE_MS = 150;
  static constexpr uint32_t TOUCH_FULL_REFRESH_HOLDOFF_MS = 2000;

  struct OptSlot { int8_t state{-1}; uint32_t ms{0}; };
  OptSlot opt_[8];
  static constexpr uint32_t OPTIMISTIC_EXPIRE_MS = 5000;
  TaskHandle_t   flush_task_handle_{nullptr};
  SemaphoreHandle_t flush_sem_{nullptr};
  SemaphoreHandle_t done_sem_{nullptr};

  // Cached battery + charger readings — every read is a blocking I2C
  // transaction; the display lambda hits these several times per render.
  // Throttle to one refresh per 10 s.
  float cached_battery_pct_{NAN};
  float cached_battery_mv_{NAN};
  float cached_battery_ma_{NAN};
  uint8_t cached_charger_status_{0};
  uint32_t battery_last_read_ms_{0};
  static constexpr uint32_t BATTERY_CACHE_MS = 10000;
};

}  // namespace t5s3_epaper
}  // namespace esphome

#endif  // USE_ESP32
