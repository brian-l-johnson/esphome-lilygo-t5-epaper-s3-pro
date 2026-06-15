#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/version.h"
#include "esphome/components/display/display_buffer.h"

#include "epdiy.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#ifdef USE_ESP32

namespace esphome {
namespace t5s3_epaper {

#if ESPHOME_VERSION_CODE >= VERSION_CODE(2023, 12, 0)
class T5S3Epaper : public display::DisplayBuffer {
#else
class T5S3Epaper : public PollingComponent, public display::DisplayBuffer {
#endif
 public:
  void setup() override;
  void update() override;
  void display();
  void clear() override { this->filled_rectangle(0, 0, this->get_width(), this->get_height(), Color(255, 255, 255)); }

  void dump_config() override;
  float get_setup_priority() const override;

  float get_battery_pct()  { return bq27220_read_reg(0x2C, false); }
  float get_battery_mv()   { return bq27220_read_reg(0x08, false); }
  float get_battery_ma()   { return bq27220_read_reg(0x14, true);  }

  void request_full_refresh() { full_refresh_ = true; }

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

  uint8_t *framebuffer_{nullptr};
  bool initialized_{false};
  bool full_refresh_{true};      // set before xSemaphoreGive; read by flush task
  bool pending_full_refresh_{false};  // latched copy read inside flush task
  uint8_t fast_refresh_count_{0};
  static constexpr uint8_t FULL_REFRESH_EVERY = 20;
  TaskHandle_t   flush_task_handle_{nullptr};
  SemaphoreHandle_t flush_sem_{nullptr};
  SemaphoreHandle_t done_sem_{nullptr};
};

}  // namespace t5s3_epaper
}  // namespace esphome

#endif  // USE_ESP32
