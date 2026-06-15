#include "t5s3_epaper.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esp_task_wdt.h"

#ifdef USE_ESP32

// ── Inline board definition using the legacy ESP-IDF I2C driver ──────────────
//
// The Frogy76 epdiy fork rewrote all board definitions to use the new
// i2c_master driver (driver/i2c_master.h).  On ESP-IDF 5.5.4 that driver
// returns ESP_ERR_INVALID_STATE from i2c_master_transmit even on a healthy
// bus (kernel bug in i2c_master.c:726-728).  The legacy driver does not have
// this problem.  The LilyGo reference firmware (epd_board_v7) also uses the
// legacy driver, so we mirror that approach here rather than patching the
// library.
//
// The board definition is inlined into this .cpp file (instead of a separate
// .c file) so that PlatformIO's CMake GLOB picks it up without requiring a
// cache-clear after the file is first added.

#include "epd_board.h"
#include "output_lcd/lcd_driver.h"

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <soc/usb_serial_jtag_reg.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_reg.h>

/* ── Pin definitions (LilyGo T5 S3 Pro) ──────────────────────────────────── */

#define S3_CFG_SCL   GPIO_NUM_40
#define S3_CFG_SDA   GPIO_NUM_39
#define S3_CFG_INTR  GPIO_NUM_38
#define S3_I2C_PORT  I2C_NUM_0

/* PCA9535 port-1 pin masks */
#define S3_PIN_OE        0x01u  /* PC10 */
#define S3_PIN_MODE      0x02u  /* PC11 */
#define S3_PIN_PWRUP     0x08u  /* PC13 */
#define S3_PIN_VCOM_CTRL 0x10u  /* PC14 */
#define S3_PIN_WAKEUP    0x20u  /* PC15 */
#define S3_PIN_PWRGOOD   0x40u  /* PC16 – input */
#define S3_PIN_INT       0x80u  /* PC17 – input */

/* PCA9535 I2C address (A2=A1=A0 tied low) */
#define S3_PCA_ADDR  0x20u

/* PCA9535 register numbers */
#define S3_PCA_IN0   0x00
#define S3_PCA_IN1   0x01
#define S3_PCA_OUT0  0x02
#define S3_PCA_OUT1  0x03
#define S3_PCA_CFG0  0x06
#define S3_PCA_CFG1  0x07

/* TPS65185 I2C address */
#define S3_TPS_ADDR  0x68u

/* TPS65185 register numbers (subset) */
#define S3_TPS_ENABLE 0x01
#define S3_TPS_VCOM1  0x03
#define S3_TPS_VCOM2  0x04
#define S3_TPS_PG     0x0F

/* LCD parallel-bus data lines (D0-D15) */
#define S3_D0  GPIO_NUM_5
#define S3_D1  GPIO_NUM_6
#define S3_D2  GPIO_NUM_7
#define S3_D3  GPIO_NUM_15
#define S3_D4  GPIO_NUM_16
#define S3_D5  GPIO_NUM_17
#define S3_D6  GPIO_NUM_18
#define S3_D7  GPIO_NUM_8
#define S3_D8  GPIO_NUM_9
#define S3_D9  GPIO_NUM_10
#define S3_D10 GPIO_NUM_11
#define S3_D11 GPIO_NUM_12
#define S3_D12 GPIO_NUM_13
#define S3_D13 GPIO_NUM_14
#define S3_D14 GPIO_NUM_21
#define S3_D15 GPIO_NUM_47

/* LCD control lines */
#define S3_CKV GPIO_NUM_48
#define S3_STH GPIO_NUM_41
#define S3_LEH GPIO_NUM_42
#define S3_STV GPIO_NUM_45
#define S3_CKH GPIO_NUM_4

/* ── Module state ─────────────────────────────────────────────────────────── */

typedef struct {
    bool pwrup;
    bool vcom_ctrl;
    bool wakeup;
} s3_ctrl_t;

static int       s3_vcom = 1600;
static s3_ctrl_t s3_ctrl;
static bool      s3_i2c_ok = false;  /* set true only when PCA9535 responds */

static lcd_bus_config_t s3_lcd_bus = {
    .data = {
        S3_D0, S3_D1, S3_D2,  S3_D3,  S3_D4,  S3_D5,  S3_D6,  S3_D7,
        S3_D8, S3_D9, S3_D10, S3_D11, S3_D12, S3_D13, S3_D14, S3_D15,
    },
    .clock       = S3_CKH,
    .ckv         = S3_CKV,
    .start_pulse = S3_STH,
    .leh         = S3_LEH,
    .stv         = S3_STV,
};

/* ── Legacy I2C helpers ───────────────────────────────────────────────────── */

static esp_err_t s3_i2c_write(uint8_t dev_addr, uint8_t reg, uint8_t value)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(S3_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static uint8_t s3_i2c_read(uint8_t dev_addr, uint8_t reg)
{
    uint8_t val = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(S3_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(S3_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return val;
}

static esp_err_t s3_pca_write(uint8_t reg, uint8_t val)
{
    return s3_i2c_write(S3_PCA_ADDR, reg, val);
}
static uint8_t s3_pca_read(uint8_t reg)
{
    return s3_i2c_read(S3_PCA_ADDR, reg);
}
static esp_err_t s3_tps_write(uint8_t reg, uint8_t val)
{
    return s3_i2c_write(S3_TPS_ADDR, reg, val);
}
static uint8_t s3_tps_read(uint8_t reg)
{
    return s3_i2c_read(S3_TPS_ADDR, reg);
}
static void s3_tps_set_vcom(unsigned vcom_mv)
{
    unsigned val = vcom_mv / 10;
    ESP_ERROR_CHECK(s3_tps_write(S3_TPS_VCOM2, (val & 0x100u) >> 8));
    ESP_ERROR_CHECK(s3_tps_write(S3_TPS_VCOM1,  val & 0xFFu));
}

/* ── Board callbacks ──────────────────────────────────────────────────────── */

static void s3_board_init(uint32_t epd_row_width)
{
    /* ── Pre-step: log key hardware registers, then wait for VDD0V3 to stabilize.
     *
     * Root-cause hypothesis: VDD0V3 (ME6217 LDO output) powers the I2C pull-up
     * resistors R18/R19 and the TPS65185/BQ27220 VDD pins.  When VDD0V3=0V at
     * boot (BQ25896 not yet providing VSYS, or ME6217 EN not yet asserted), the
     * pull-ups tie SDA/SCL to GND, and the ESD protection diodes inside
     * TPS65185/BQ27220 (pad→VDD=0V, forward-biased) clamp SDA/SCL to ~0.5V —
     * below the GPIO input-HIGH threshold (~1.65V).  A push-pull GPIO output
     * cannot overcome a forward-biased ESD clamp.
     *
     * Fix: wait up to 5 s for the lines to go idle-high, polling every 100 ms.
     * This is only needed on first-boot / soft-reset before VSYS/VDD0V3 are
     * stable; subsequent boots after a full power-on will be fast. */
    {
        uint32_t usb_conf0 = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
        ESP_LOGI("epdiy", "USB_SERIAL_JTAG CONF0=0x%08X (BRIDGE_EN bit16=%d)",
                 (unsigned)usb_conf0,
                 (int)((usb_conf0 >> 16) & 1));
    }

    /* ── Diagnostic: BRIDGE_EN=1 test ───────────────────────────────────────
     * Hypothesis: USB_SERIAL_JTAG peripheral is driving JTAG TDO directly to
     * GPIO40 pad via a hardware bypass that ignores the IOMUX MCU_SEL setting.
     * When BRIDGE_EN=0 (default) TDO is driven LOW (idle JTAG state), clamping
     * SCL to 0V with <5Ω impedance — explains why GPIO_DRIVE_CAP_3 can't win.
     *
     * When BRIDGE_EN=1 the TDI/TDO/TCK/TMS signals are re-routed through the
     * GPIO matrix; MTDO (GPIO40) becomes an INPUT instead of an OUTPUT.
     * If GPIO40 goes HIGH with BRIDGE_EN=1, the USB_SERIAL_JTAG peripheral
     * was the active driver and permanently keeping BRIDGE_EN=1 (or disabling
     * the USB_SERIAL_JTAG peripheral) will free SCL for normal I2C use.
     *
     * Do NOT gpio_reset_pin(SCL) here — we want the IOMUX state unchanged so
     * we can compare before/after BRIDGE_EN purely on the peripheral side. */
    {
        /* Enable internal pull-up on GPIO40 without touching IOMUX routing. */
        gpio_set_pull_mode(S3_CFG_SCL, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(S3_CFG_SDA, GPIO_PULLUP_ONLY);
        esp_rom_delay_us(100);
        int scl_bridge0 = gpio_get_level(S3_CFG_SCL);
        int sda_bridge0 = gpio_get_level(S3_CFG_SDA);

        /* Set BRIDGE_EN=1: MTDO becomes an input, USB JTAG stops driving it. */
        SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG,
                          USB_SERIAL_JTAG_USB_JTAG_BRIDGE_EN_M);
        esp_rom_delay_us(100);
        int scl_bridge1 = gpio_get_level(S3_CFG_SCL);
        int sda_bridge1 = gpio_get_level(S3_CFG_SDA);

        ESP_LOGI("epdiy",
                 "BRIDGE_EN=0: SCL=%d SDA=%d  |  BRIDGE_EN=1: SCL=%d SDA=%d",
                 scl_bridge0, sda_bridge0, scl_bridge1, sda_bridge1);

        if (scl_bridge1 && !scl_bridge0) {
            ESP_LOGW("epdiy",
                     "USB_SERIAL_JTAG was driving SCL (GPIO40) LOW! "
                     "Keeping BRIDGE_EN=1 to free SCL for I2C.");
            /* Leave BRIDGE_EN=1 — MTDO stays an input, SCL is now free. */
        } else {
            /* BRIDGE_EN did not help — restore original state. */
            CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG,
                                USB_SERIAL_JTAG_USB_JTAG_BRIDGE_EN_M);
            ESP_LOGI("epdiy",
                     "BRIDGE_EN=1 did not free SCL — not a USB_JTAG issue. "
                     "BRIDGE_EN restored to 0.");
        }
    }

    /* Configure SDA/SCL as INPUT+PULLUP early so we can poll them. */
    gpio_reset_pin(S3_CFG_SDA);
    gpio_set_direction(S3_CFG_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(S3_CFG_SDA, GPIO_PULLUP_ONLY);
    gpio_reset_pin(S3_CFG_SCL);
    gpio_set_direction(S3_CFG_SCL, GPIO_MODE_INPUT);
    gpio_set_pull_mode(S3_CFG_SCL, GPIO_PULLUP_ONLY);

    ESP_LOGI("epdiy", "Waiting for VDD0V3 / I2C pull-ups to come up...");
    int wait_ms = 0;
    while (wait_ms < 5000) {
        int sda = gpio_get_level(S3_CFG_SDA);
        int scl = gpio_get_level(S3_CFG_SCL);
        if (sda && scl) {
            ESP_LOGI("epdiy", "  I2C bus idle-high after %d ms (SDA=%d SCL=%d)",
                     wait_ms, sda, scl);
            break;
        }
        if (wait_ms % 500 == 0) {
            /* Every 500 ms: push-pull test with both default and max drive
             * strength.  A stuck slave holds SCL via open-drain NFET (~20Ω).
             * Default drive (DRV=2, ~40mA, ~25Ω) gives only 1.47V into 20Ω —
             * below the 1.65V threshold.  Max drive (DRV=3, ~80mA, ~5Ω) gives
             * 2.64V — safely above threshold.  Seeing DRV3=1 but DRV2=0 would
             * confirm a clock-stretching slave and guide the recovery approach. */
            gpio_set_direction(S3_CFG_SCL, GPIO_MODE_OUTPUT);
            gpio_set_drive_capability(S3_CFG_SCL, GPIO_DRIVE_CAP_2);
            gpio_set_level(S3_CFG_SCL, 1);
            esp_rom_delay_us(50);
            int scl_drv2 = gpio_get_level(S3_CFG_SCL);

            gpio_set_drive_capability(S3_CFG_SCL, GPIO_DRIVE_CAP_3);
            gpio_set_level(S3_CFG_SCL, 1);
            esp_rom_delay_us(50);
            int scl_drv3 = gpio_get_level(S3_CFG_SCL);

            gpio_set_level(S3_CFG_SCL, 0);   /* release (low) before input */
            gpio_set_direction(S3_CFG_SCL, GPIO_MODE_INPUT);
            gpio_set_pull_mode(S3_CFG_SCL, GPIO_PULLUP_ONLY);

            ESP_LOGI("epdiy", "  t=%dms: SDA=%d SCL(in)=%d "
                     "SCL(DRV2-hi)=%d SCL(DRV3-hi)=%d IN1=0x%08X",
                     wait_ms, sda, scl, scl_drv2, scl_drv3,
                     (unsigned)REG_READ(GPIO_IN1_REG));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
    }
    if (wait_ms >= 5000) {
        ESP_LOGE("epdiy", "VDD0V3 / I2C lines did not come up within 5 s — "
                 "hardware power issue (check BQ25896/ME6217)");
    }

    /* ── Step 0: delete any pre-existing I2C driver to reset the peripheral.
     * After an OTA soft-reset the previous firmware's I2C driver (and any
     * in-flight transaction) may still be active in the peripheral.  Deleting
     * it aborts the transaction and releases the GPIO matrix routing so that
     * our gpio_reset_pin() calls below have full effect. */
    i2c_driver_delete(S3_I2C_PORT);   /* ignore error if not installed */

    /* ── Step 1: release GPIO holds from the previous firmware. */
    gpio_hold_dis(S3_CFG_SDA);
    gpio_hold_dis(S3_CFG_SCL);
    gpio_hold_dis(S3_CKH);
    gpio_deep_sleep_hold_dis();

    /* ── Step 2: hardware-reset the GT911 touch controller (GPIO9 = TOUCH_RST).
     * The GT911 can hold SDA low when caught mid-transaction.  Pulsing RST
     * clears its internal state.  The GT911 datasheet requires ≥ 5 ms after
     * RST deasserts before the device is ready; use 100 ms to be safe. */
    gpio_reset_pin(GPIO_NUM_9);
    gpio_set_direction(GPIO_NUM_9, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_9, 0);           /* assert reset (active-low) */
    vTaskDelay(pdMS_TO_TICKS(10));           /* hold 10 ms */
    gpio_set_level(GPIO_NUM_9, 1);           /* deassert reset */
    vTaskDelay(pdMS_TO_TICKS(100));          /* 100 ms for full GT911 init */

    /* ── Step 3: read bus state with both lines as INPUT+PULLUP. */
    gpio_reset_pin(S3_CFG_SDA);
    gpio_set_direction(S3_CFG_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(S3_CFG_SDA, GPIO_PULLUP_ONLY);
    gpio_reset_pin(S3_CFG_SCL);
    gpio_set_direction(S3_CFG_SCL, GPIO_MODE_INPUT);
    gpio_set_pull_mode(S3_CFG_SCL, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_LOGI("epdiy", "Pre-I2C GPIO: SDA(GPIO%d)=%d SCL(GPIO%d)=%d",
             S3_CFG_SDA, gpio_get_level(S3_CFG_SDA),
             S3_CFG_SCL, gpio_get_level(S3_CFG_SCL));

    /* ── Step 4: I2C bus recovery.
     * Trigger when either line is low (SCL=0 = clock stretch; SDA=0 = data).
     * 32 pulses covers up to a full 2-byte transaction at any bit position.
     * We drive SCL in push-pull OUTPUT mode to override any slave clock stretch. */
    if (!gpio_get_level(S3_CFG_SDA) || !gpio_get_level(S3_CFG_SCL)) {
        ESP_LOGW("epdiy", "Bus stuck (SDA=%d SCL=%d) — recovering",
                 gpio_get_level(S3_CFG_SDA), gpio_get_level(S3_CFG_SCL));
        /* Use maximum drive strength (DRV3 ≈ 80mA, ~5Ω) for SCL recovery.
         * A clock-stretching slave holds SCL via open-drain NFET (~20Ω).
         * With DRV3: V_SCL = 3.3V × 20/(5+20) = 2.64V → reads HIGH.
         * With default DRV2 (~25Ω): V_SCL = 1.47V → reads LOW (false fail). */
        gpio_set_direction(S3_CFG_SCL, GPIO_MODE_OUTPUT);
        gpio_set_drive_capability(S3_CFG_SCL, GPIO_DRIVE_CAP_3);
        gpio_set_direction(S3_CFG_SDA, GPIO_MODE_INPUT);
        gpio_set_pull_mode(S3_CFG_SDA, GPIO_PULLUP_ONLY);
        /* Log GPIO38 (PCA9535 INT, pulled to VDD3V3) as a GPIO sanity check. */
        gpio_set_direction(GPIO_NUM_38, GPIO_MODE_INPUT);
        gpio_set_pull_mode(GPIO_NUM_38, GPIO_PULLUP_ONLY);
        ESP_LOGI("epdiy", "GPIO38(INT/VDD3V3-pullup)=%d", gpio_get_level(GPIO_NUM_38));

        bool sda_free = false;
        for (int i = 0; i < 32; i++) {
            gpio_set_level(S3_CFG_SCL, 0);
            esp_rom_delay_us(100);
            gpio_set_level(S3_CFG_SCL, 1);
            esp_rom_delay_us(100);
            /* Read SCL while we're actively driving it HIGH (push-pull).
             * If this reads 0, something is overcoming our GPIO output — not
             * normal open-drain clock-stretching. */
            int scl_driven = gpio_get_level(S3_CFG_SCL);
            int sda_val    = gpio_get_level(S3_CFG_SDA);
            if (i < 5) {
                ESP_LOGI("epdiy", "  pulse %2d: SCL(driven-hi)=%d SDA=%d",
                         i, scl_driven, sda_val);
            }
            if (sda_val) {
                ESP_LOGI("epdiy", "SDA released after %d pulses", i + 1);
                sda_free = true;
                break;
            }
        }
        /* Generate STOP condition: SDA low → SCL high → SDA high */
        gpio_set_direction(S3_CFG_SDA, GPIO_MODE_OUTPUT);
        gpio_set_level(S3_CFG_SDA, 0);
        esp_rom_delay_us(10);
        gpio_set_level(S3_CFG_SCL, 1);
        esp_rom_delay_us(10);
        gpio_set_level(S3_CFG_SDA, 1);
        esp_rom_delay_us(10);
        /* Return both to inputs and let pull-ups settle. */
        gpio_set_direction(S3_CFG_SDA, GPIO_MODE_INPUT);
        gpio_set_pull_mode(S3_CFG_SDA, GPIO_PULLUP_ONLY);
        gpio_set_direction(S3_CFG_SCL, GPIO_MODE_INPUT);
        gpio_set_pull_mode(S3_CFG_SCL, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI("epdiy", "After recovery: SDA=%d SCL=%d (SDA released=%d)",
                 gpio_get_level(S3_CFG_SDA), gpio_get_level(S3_CFG_SCL), (int)sda_free);
    }

    /* ── Step 5: install I2C driver. */
    i2c_config_t conf = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = S3_CFG_SDA,
        .scl_io_num    = S3_CFG_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master        = { .clk_speed = 100000 },
        .clk_flags     = 0,
    };
    i2c_param_config(S3_I2C_PORT, &conf);
    i2c_driver_install(S3_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    esp_rom_delay_us(2000);

    /* I2C device scan — only log found devices. */
    ESP_LOGI("epdiy", "I2C scan:");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t probe = i2c_cmd_link_create();
        i2c_master_start(probe);
        i2c_master_write_byte(probe, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(probe);
        esp_err_t r = i2c_master_cmd_begin(S3_I2C_PORT, probe, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(probe);
        if (r == ESP_OK) {
            ESP_LOGI("epdiy", "  0x%02X found", addr);
            found++;
        }
    }
    ESP_LOGI("epdiy", "I2C scan done: %d device(s)", found);
    if (found == 0)
        ESP_LOGE("epdiy", "No I2C devices found — power-cycle the device to reset stuck slaves.");

    s3_ctrl.pwrup     = false;
    s3_ctrl.vcom_ctrl = false;
    s3_ctrl.wakeup    = false;

    gpio_set_direction(S3_CFG_INTR, GPIO_MODE_INPUT);
    gpio_set_intr_type(S3_CFG_INTR, GPIO_INTR_NEGEDGE);
    /* Ignore error: GPIO ISR service may already be installed by ESPHome. */
    (void)gpio_install_isr_service(ESP_INTR_FLAG_EDGE);

    /* Port 1: PWRGOOD (bit6) and INT (bit7) are inputs; rest are outputs. */
    esp_err_t pca_ret = s3_pca_write(S3_PCA_CFG1, S3_PIN_PWRGOOD | S3_PIN_INT);
    ESP_LOGI("epdiy", "PCA9535 CFG1 write: %s", esp_err_to_name(pca_ret));
    if (pca_ret != ESP_OK) {
        ESP_LOGE("epdiy", "PCA9535 not responding — display will not work");
        return;  /* s3_i2c_ok stays false */
    }
    s3_i2c_ok = true;
    /* Port 0: all outputs. */
    ESP_ERROR_CHECK(s3_pca_write(S3_PCA_CFG0, 0x00));
    /* Set port-0 outputs high (safe/inactive state). */
    ESP_ERROR_CHECK(s3_pca_write(S3_PCA_OUT0, 0xFF));

    const EpdDisplay_t *display = epd_get_display();
    LcdEpdConfig_t lcd_cfg = {
        // 8 MHz: safe for 32B DMA cache lines; check_cache_configuration() is
        // patched to not halve the clock so this value is used as-is.
        .pixel_clock      = 8000000u,
        .ckv_high_time    = 60,
        .line_front_porch = 4,
        .le_high_time     = 4,
        .bus_width        = display->bus_width,
        .bus              = s3_lcd_bus,
    };
    epd_lcd_init(&lcd_cfg, display->width, display->height);
}

static void s3_board_deinit(void)
{
    epd_lcd_deinit();

    s3_pca_write(S3_PCA_CFG1,
                 S3_PIN_PWRGOOD | S3_PIN_INT | S3_PIN_VCOM_CTRL | S3_PIN_PWRUP);

    int tries = 0;
    while (((s3_pca_read(S3_PCA_IN1) & 0xC0u) != 0x80u)) {
        if (tries++ >= 50) {
            ESP_LOGE("epdiy", "failed to shut down TPS65185!");
            break;
        }
        vTaskDelay(1);
    }
    vTaskDelay(50);
    (void)s3_pca_read(S3_PCA_IN0);
    (void)s3_pca_read(S3_PCA_IN1);

    i2c_driver_delete(S3_I2C_PORT);
    gpio_uninstall_isr_service();
}

static void s3_board_set_ctrl(epd_ctrl_state_t *state,
                               const epd_ctrl_state_t *const mask)
{
    if (!s3_i2c_ok) return;
    if (mask->ep_output_enable || mask->ep_mode || mask->ep_stv) {
        uint8_t val = 0;
        if (state->ep_output_enable) val |= S3_PIN_OE;
        if (state->ep_mode)          val |= S3_PIN_MODE;
        if (s3_ctrl.pwrup)           val |= S3_PIN_PWRUP;
        if (s3_ctrl.vcom_ctrl)       val |= S3_PIN_VCOM_CTRL;
        if (s3_ctrl.wakeup)          val |= S3_PIN_WAKEUP;
        esp_err_t r = s3_pca_write(S3_PCA_OUT1, val);
        if (r != ESP_OK)
            ESP_LOGW("epdiy", "set_ctrl PCA write failed: %s", esp_err_to_name(r));
    }
}

static void s3_board_poweron(epd_ctrl_state_t *state)
{
    if (!s3_i2c_ok) return;
    epd_ctrl_state_t mask = {
        .ep_output_enable = true, .ep_mode = true, .ep_stv = true
    };
    state->ep_stv           = true;
    state->ep_mode          = false;
    state->ep_output_enable = true;

    s3_ctrl.wakeup = true;
    s3_board_set_ctrl(state, &mask);
    s3_ctrl.pwrup = true;
    s3_board_set_ctrl(state, &mask);
    s3_ctrl.vcom_ctrl = true;
    s3_board_set_ctrl(state, &mask);

    vTaskDelay(1);
    while (!(s3_pca_read(S3_PCA_IN1) & S3_PIN_PWRGOOD)) {}

    ESP_ERROR_CHECK(s3_tps_write(S3_TPS_ENABLE, 0x3F));
    s3_tps_set_vcom(s3_vcom);

    state->ep_sth = true;
    mask = epd_ctrl_state_t{ .ep_sth = true };
    s3_board_set_ctrl(state, &mask);

    int tries = 0;
    while ((s3_tps_read(S3_TPS_PG) & 0xFAu) != 0xFAu) {
        if (tries++ >= 500) {
            ESP_LOGE("epdiy", "Power enable failed! PG: 0x%02X",
                     s3_tps_read(S3_TPS_PG));
            return;
        }
        vTaskDelay(1);
    }
}

static void s3_board_poweroff(epd_ctrl_state_t *state)
{
    if (!s3_i2c_ok) return;
    epd_ctrl_state_t mask = {
        .ep_output_enable = true, .ep_mode = true, .ep_stv = true
    };
    s3_ctrl.vcom_ctrl       = false;
    s3_ctrl.pwrup           = false;
    state->ep_stv           = false;
    state->ep_output_enable = false;
    state->ep_mode          = false;
    s3_board_set_ctrl(state, &mask);
    vTaskDelay(1);
    s3_ctrl.wakeup = false;
    s3_board_set_ctrl(state, &mask);
}

static float s3_board_temperature(void) { return 20.0f; }
static void  s3_set_vcom(int mv)        { s3_vcom = mv; }

/* ── Exported board definition ────────────────────────────────────────────── */

static const EpdBoardDefinition epd_board_s3_legacy = {
    .init               = s3_board_init,
    .deinit             = s3_board_deinit,
    .set_ctrl           = s3_board_set_ctrl,
    .poweron            = s3_board_poweron,
    .measure_vcom       = nullptr,
    .poweroff           = s3_board_poweroff,
    .set_vcom           = s3_set_vcom,
    .get_temperature    = s3_board_temperature,
    .gpio_set_direction = nullptr,
    .gpio_read          = nullptr,
    .gpio_write         = nullptr,
};

/* ── ESPHome component ────────────────────────────────────────────────────── */

namespace esphome {
namespace t5s3_epaper {

static const char *const TAG = "t5s3_epaper";

void T5S3Epaper::setup() {
  ESP_LOGCONFIG(TAG, "Setting up T5 E-Paper S3 Pro...");

  // Sentinel so ESPHome's null-guards don't trip.
  this->buffer_ = static_cast<uint8_t *>(malloc(1));
  if (!this->buffer_) {
    ESP_LOGE(TAG, "Sentinel buffer allocation failed");
    this->mark_failed();
    return;
  }

  // epd_init() can take several seconds (LCD peripheral + I2C power sequencing).
  // Only unsubscribe from the task WDT if we're actually subscribed.
  bool wdt_subscribed = (esp_task_wdt_delete(xTaskGetCurrentTaskHandle()) == ESP_OK);
  epd_init(&epd_board_s3_legacy, &ED047TC1, EPD_LUT_1K);
  if (wdt_subscribed) {
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    esp_task_wdt_reset();
  }

  // Framebuffer is 4bpp packed (2 pixels per byte) and must live in PSRAM.
  int fb_size = epd_width() / 2 * epd_height();
  this->framebuffer_ = static_cast<uint8_t *>(heap_caps_calloc(fb_size, 1, MALLOC_CAP_SPIRAM));
  if (!this->framebuffer_) {
    ESP_LOGE(TAG, "Framebuffer allocation failed (%d bytes PSRAM)", fb_size);
    this->mark_failed();
    return;
  }

  this->flush_sem_ = xSemaphoreCreateBinary();
  this->done_sem_  = xSemaphoreCreateBinary();

  xTaskCreatePinnedToCore(
      T5S3Epaper::flush_task_,
      "epd_flush",
      8192,
      this,
      1,                  // same priority as loopTask — yields to it between epdiy calls
      &this->flush_task_handle_,
      0);                 // core 0 — loopTask runs on core 1, so flush never blocks the main loop

  this->initialized_ = true;
  ESP_LOGCONFIG(TAG, "T5S3Epaper ready (%dx%d, framebuffer=%d bytes PSRAM) [lilygo_board_s3]",
                epd_width(), epd_height(), fb_size);
}

float T5S3Epaper::get_setup_priority() const { return setup_priority::PROCESSOR; }

void T5S3Epaper::flush_task_(void *arg) {
  auto *self = static_cast<T5S3Epaper *>(arg);
  for (;;) {
    xSemaphoreTake(self->flush_sem_, portMAX_DELAY);

    bool do_full = self->pending_full_refresh_;
    EpdRect area = epd_full_screen();
    uint32_t t0 = millis();

    epd_poweron();
    if (do_full) {
      epd_clear();
      EpdDrawError err = epd_draw_base(
          area, self->framebuffer_, area,
          static_cast<EpdDrawMode>(MODE_GL16 | PREVIOUSLY_WHITE | MODE_PACKING_2PPB),
          25, nullptr, nullptr, epd_get_display()->default_waveform);
      if (err != EPD_DRAW_SUCCESS)
        ESP_LOGE("t5s3_epaper", "epd_draw_base (GL16) failed: 0x%x", err);
      ESP_LOGD("t5s3_epaper", "Full GL16 refresh in %u ms", millis() - t0);
    } else {
      EpdDrawError err = epd_draw_base(
          area, self->framebuffer_, area,
          static_cast<EpdDrawMode>(MODE_DU | PREVIOUSLY_WHITE | MODE_PACKING_2PPB),
          25, nullptr, nullptr, epd_get_display()->default_waveform);
      if (err != EPD_DRAW_SUCCESS)
        ESP_LOGE("t5s3_epaper", "epd_draw_base (DU) failed: 0x%x", err);
      ESP_LOGD("t5s3_epaper", "Fast DU refresh in %u ms", millis() - t0);
    }
    epd_poweroff();

    xSemaphoreGive(self->done_sem_);
  }
}

void T5S3Epaper::update() {
  if (!initialized_) return;

  // Render the framebuffer on the main loop (fast — just pixel writes).
  this->do_update_();
  this->display();
}

void T5S3Epaper::display() {
  if (!initialized_) return;

  // Auto-promote to full refresh every FULL_REFRESH_EVERY fast updates
  // to clear accumulated DU ghosting.
  if (!full_refresh_) {
    if (++fast_refresh_count_ >= FULL_REFRESH_EVERY) {
      full_refresh_ = true;
      fast_refresh_count_ = 0;
    }
  } else {
    fast_refresh_count_ = 0;
  }

  // Latch the flag for the flush task, then clear it for next time.
  pending_full_refresh_ = full_refresh_;
  full_refresh_ = false;

  // Only queue one flush at a time — if one is already pending, skip.
  if (xSemaphoreGive(this->flush_sem_) == pdTRUE) {
    ESP_LOGD(TAG, "Requesting %s refresh...", pending_full_refresh_ ? "full GL16" : "fast DU");
  }
}

void HOT T5S3Epaper::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= 960 || y < 0 || y >= 540) return;

  uint8_t luma = static_cast<uint8_t>(
      (static_cast<uint16_t>(color.red)   * 76u +
       static_cast<uint16_t>(color.green) * 150u +
       static_cast<uint16_t>(color.blue)  * 30u) >> 8u);

  epd_draw_pixel(x, y, luma, this->framebuffer_);
}

float T5S3Epaper::bq27220_read_reg(uint8_t reg, bool is_signed) {
  uint8_t buf[2] = {};

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (0x55u << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg, true);
  i2c_master_stop(cmd);
  esp_err_t ret = i2c_master_cmd_begin(S3_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK) return NAN;

  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (0x55u << 1) | I2C_MASTER_READ, true);
  i2c_master_read(cmd, buf, 1, I2C_MASTER_ACK);
  i2c_master_read_byte(cmd, buf + 1, I2C_MASTER_NACK);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(S3_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK) return NAN;

  uint16_t raw = (uint16_t)(buf[1] << 8 | buf[0]);
  return is_signed ? (float)(int16_t)raw : (float)raw;
}

void T5S3Epaper::dump_config() {
  LOG_DISPLAY("", "T5S3Epaper", this);
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace t5s3_epaper
}  // namespace esphome

#endif  // USE_ESP32
