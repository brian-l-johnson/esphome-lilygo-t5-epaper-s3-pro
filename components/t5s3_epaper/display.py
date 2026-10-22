import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import (
    CONF_ID,
    CONF_LAMBDA,
    CONF_PAGES,
)
from . import t5s3_ns

DEPENDENCIES = ["esp32"]

T5S3Epaper = t5s3_ns.class_(
    "T5S3Epaper", cg.PollingComponent, display.DisplayBuffer
)

CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(T5S3Epaper),
        }
    ).extend(cv.polling_component_schema("60s")),
    cv.has_at_most_one_key(CONF_PAGES, CONF_LAMBDA),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))

    cg.add_build_flag("-DBOARD_HAS_PSRAM")

    # Frogy76's epdiy fork — fixes ESP-IDF 5.5.x compatibility issues in the
    # upstream library (gpio_hal_iomux_func_sel removal, RCC atomic env, GDMA API).
    # Uses lilygo_board_s3 + ED047TC1 (4.7", 960x540, parallel interface, PCA9535+TPS65185).
    # Pinned to a known-good commit so upstream pushes can't break builds.
    cg.add_library("epdiy", None, "https://github.com/Frogy76/epdiy#148d877db9bc5eabd06bc07ad8a64bbf56a1962d")
