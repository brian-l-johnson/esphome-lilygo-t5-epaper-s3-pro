import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import touchscreen
from esphome.const import CONF_ID, CONF_INTERRUPT_PIN, CONF_RESET_PIN
from esphome import pins

from .. import t5s3_ns

DEPENDENCIES = ["esp32"]

T5S3Touch = t5s3_ns.class_(
    "T5S3Touch",
    touchscreen.Touchscreen,
)

CONF_CALIBRATION = "calibration"
CONF_X_MIN = "x_min"
CONF_X_MAX = "x_max"
CONF_Y_MIN = "y_min"
CONF_Y_MAX = "y_max"

CALIBRATION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_X_MIN): cv.int_,
        cv.Required(CONF_X_MAX): cv.int_,
        cv.Required(CONF_Y_MIN): cv.int_,
        cv.Required(CONF_Y_MAX): cv.int_,
    }
)

CONFIG_SCHEMA = touchscreen.TOUCHSCREEN_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(T5S3Touch),
        cv.Optional(CONF_INTERRUPT_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_CALIBRATION): CALIBRATION_SCHEMA,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await touchscreen.register_touchscreen(var, config)

    if interrupt_pin := config.get(CONF_INTERRUPT_PIN):
        cg.add(var.set_interrupt_pin(await cg.gpio_pin_expression(interrupt_pin)))
    if reset_pin := config.get(CONF_RESET_PIN):
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(reset_pin)))
    if cal := config.get(CONF_CALIBRATION):
        cg.add(var.set_cal_raw_x_min(cal[CONF_X_MIN]))
        cg.add(var.set_cal_raw_x_max(cal[CONF_X_MAX]))
        cg.add(var.set_cal_raw_y_min(cal[CONF_Y_MIN]))
        cg.add(var.set_cal_raw_y_max(cal[CONF_Y_MAX]))
