"""Renergy RN8209C/RN8209D - single-phase energy metering IC with two current
channels.

Connected over SPI (the chip's IS pin tied high). One chip measures a single
shared voltage plus two independent current channels, A and B, each with its
own active power register.

This block defines the bus (SCLK/MOSI/MISO). The chips themselves are declared
as the "rn8209d" sensor platform, each with its own cs_pin.

The bus is implemented in software (bit-banging) because ESPHome does not offer
its spi component on the LibreTiny platform.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_CLK_PIN, CONF_ID, CONF_MISO_PIN, CONF_MOSI_PIN

CODEOWNERS = ["@sl1288"]
MULTI_CONF = True

rn8209d_ns = cg.esphome_ns.namespace("rn8209d")
RN8209DBus = rn8209d_ns.class_("RN8209DBus", cg.Component)

CONF_CLOCK_DELAY_US = "clock_delay_us"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RN8209DBus),
        cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_schema,
        cv.Required(CONF_MOSI_PIN): pins.internal_gpio_output_pin_schema,
        cv.Required(CONF_MISO_PIN): pins.internal_gpio_input_pin_schema,
        # Half clock period. 4 us is roughly 125 kHz and unproblematic;
        # increase it if transfers turn out unreliable.
        cv.Optional(CONF_CLOCK_DELAY_US, default=4): cv.int_range(min=1, max=500),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    clk = await cg.gpio_pin_expression(config[CONF_CLK_PIN])
    cg.add(var.set_clk_pin(clk))
    mosi = await cg.gpio_pin_expression(config[CONF_MOSI_PIN])
    cg.add(var.set_mosi_pin(mosi))
    miso = await cg.gpio_pin_expression(config[CONF_MISO_PIN])
    cg.add(var.set_miso_pin(miso))
    cg.add(var.set_clock_delay(config[CONF_CLOCK_DELAY_US]))
