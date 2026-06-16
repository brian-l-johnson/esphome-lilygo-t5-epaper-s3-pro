#include "t5s3_touch.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace t5s3_epaper {

static const char *const TAG = "t5s3_touch";

static const i2c_port_t GT911_I2C_PORT = I2C_NUM_0;

// GT911 register addresses
static const uint16_t GT911_STATUS_REG  = 0x814E;
static const uint16_t GT911_TOUCH0_REG  = 0x814F;
static const uint16_t GT911_MAX_XY_REG  = 0x8048;
static const uint16_t GT911_CONFIG_REG  = 0x8047;

static const size_t GT911_MAX_TOUCHES   = 5;
static const size_t GT911_TOUCH_BYTES   = 8;  // bytes per touch point

// ── Low-level I2C helpers using the same legacy driver as the board init ──────

bool T5S3Touch::gt911_read_(uint16_t reg, uint8_t *buf, size_t len) {
  uint8_t reg_buf[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (this->gt911_addr_ << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write(cmd, reg_buf, 2, true);
  i2c_master_stop(cmd);
  esp_err_t ret = i2c_master_cmd_begin(GT911_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK) return false;

  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (this->gt911_addr_ << 1) | I2C_MASTER_READ, true);
  if (len > 1)
    i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
  i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(GT911_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret == ESP_OK;
}

bool T5S3Touch::gt911_write_(uint16_t reg, uint8_t val) {
  uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (this->gt911_addr_ << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write(cmd, buf, 3, true);
  i2c_master_stop(cmd);
  esp_err_t ret = i2c_master_cmd_begin(GT911_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret == ESP_OK;
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void T5S3Touch::setup() {
  // GT911 I2C address depends on INT pin state during reset:
  //   INT low  during reset → 0x5D (normal)
  //   INT high during reset → 0x14 (alternate)
  // After a USB flash the esptool reset can leave INT in either state.
  // Try both addresses, retrying for up to 2s total.
  static const uint8_t addrs[] = {0x5D, 0x14};
  uint8_t id[4] = {};
  bool ok = false;

  for (int attempt = 0; attempt < 20 && !ok; attempt++) {
    vTaskDelay(pdMS_TO_TICKS(100));
    for (uint8_t addr : addrs) {
      this->gt911_addr_ = addr;
      if (this->gt911_read_(0x8140, id, 4)) {
        ESP_LOGI(TAG, "GT911 found at 0x%02X on attempt %d", addr, attempt + 1);
        ok = true;
        break;
      }
    }
  }

  if (!ok) {
    ESP_LOGE(TAG, "GT911 not responding on 0x5D or 0x14 after 2s");
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "GT911 product ID: %c%c%c%c", id[0], id[1], id[2], id[3]);

  // Read max X/Y from GT911 config so the touchscreen base class can map
  // raw coordinates to display pixels without a calibration block in YAML.
  uint8_t xy[4] = {};
  if (this->gt911_read_(GT911_MAX_XY_REG, xy, 4)) {
    uint16_t x_max = (uint16_t)(xy[1] << 8 | xy[0]);
    uint16_t y_max = (uint16_t)(xy[3] << 8 | xy[2]);
    ESP_LOGI(TAG, "GT911 raw max coords from chip: x=%d y=%d", x_max, y_max);
  }
  // We hand the base class already-mapped display-space coords (see
  // update_touches), and want its normalize_/scale step to be an identity.
  // Setting raw_max = display_size makes normalize_(val=display_size) hit
  // the >= max_val branch returning 0xFFF, which then scales back to
  // display_size - 1 exactly. Any value in between round-trips with at
  // most ±1 px rounding.
  this->x_raw_min_ = 0;
  this->x_raw_max_ = 540;
  this->y_raw_min_ = 0;
  this->y_raw_max_ = 960;

  // GPIO38 is shared between PCA9535 INT and GT911 INT — using it as an
  // interrupt source causes conflicts with the board init ISR handler.
  // Use polling mode instead: store_.init stays false so the base class
  // loop sets store_.touched = true on every update interval.

  this->setup_done_ = true;
  ESP_LOGI(TAG, "GT911 touch ready");
}

// ── update_touches — called by the Touchscreen base class loop ────────────────

void T5S3Touch::update_touches() {
  if (!this->setup_done_) {
    this->skip_update_ = true;
    return;
  }

  uint8_t status = 0;
  if (!this->gt911_read_(GT911_STATUS_REG, &status, 1)) {
    this->skip_update_ = true;
    return;
  }

  // Bit 7 = buffer ready; bits 0-3 = number of touch points.
  if (!(status & 0x80)) {
    // No new data — clear the status register and skip.
    this->gt911_write_(GT911_STATUS_REG, 0x00);
    this->skip_update_ = true;
    return;
  }

  uint8_t num_touches = status & 0x0F;
  if (num_touches > GT911_MAX_TOUCHES) {
    this->gt911_write_(GT911_STATUS_REG, 0x00);
    this->skip_update_ = true;
    return;
  }

  if (num_touches == 0) {
    // Finger lifted — base class handles release. Clear status so the chip
    // can report the next touch.
    this->gt911_write_(GT911_STATUS_REG, 0x00);
    return;
  }

  uint8_t points[GT911_MAX_TOUCHES * GT911_TOUCH_BYTES] = {};
  if (!this->gt911_read_(GT911_TOUCH0_REG, points, num_touches * GT911_TOUCH_BYTES)) {
    this->skip_update_ = true;
    return;
  }

  // Clear status AFTER reading the points — otherwise the chip can latch a
  // new sample in the gap between our clear and our read, and we'd see
  // half-updated point data.
  this->gt911_write_(GT911_STATUS_REG, 0x00);

  for (uint8_t i = 0; i < num_touches; i++) {
    const uint8_t *p = points + i * GT911_TOUCH_BYTES;
    uint8_t  id    = p[0];
    uint16_t raw_x = (uint16_t)(p[2] << 8 | p[1]);
    uint16_t raw_y = (uint16_t)(p[4] << 8 | p[3]);

    // Map chip coords to screen pixel coords for rotation=270, portrait 540x960.
    // Calibration constants are configurable via YAML — see set_cal_raw_*.
    int screen_x = ((int)raw_x - cal_raw_x_min_) * 540 / (cal_raw_x_max_ - cal_raw_x_min_);
    int screen_y = ((int)raw_y - cal_raw_y_min_) * 960 / (cal_raw_y_max_ - cal_raw_y_min_);
    screen_x = std::max(0, std::min(539, screen_x));
    screen_y = std::max(0, std::min(959, screen_y));

    this->add_raw_touch_position_(id, (int16_t)screen_x, (int16_t)screen_y);
    ESP_LOGV(TAG, "Touch %d: id=%d raw(%d,%d) screen(%d,%d)", i, id, raw_x, raw_y, screen_x, screen_y);
  }
}

void T5S3Touch::dump_config() {
  ESP_LOGCONFIG(TAG, "T5S3 Touch (GT911): polling mode, 50ms interval");
}

}  // namespace t5s3_epaper
}  // namespace esphome

#endif  // USE_ESP32
