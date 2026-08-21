"""Pin scanner for RN8209C/D metering chips on a software SPI bus.

Tries every combination of SCLK / SDI(MOSI) / CS from the given GPIOs and reads
the device ID register (0x7F) for each. Every pin not currently driven as an
output is sampled at the same time as an SDO(MISO) candidate, which removes one
dimension from the search space.

Hits are written to the log (expected value 0x820900).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@sl1288"]

rn8209_scan_ns = cg.esphome_ns.namespace("rn8209_scan")
RN8209Scan = rn8209_scan_ns.class_("RN8209Scan", cg.Component)

CONF_PINS = "pins"
CONF_CLOCK_DELAY_US = "clock_delay_us"
CONF_EXPECTED_ID = "expected_id"

# All 19 GPIOs of the BK7231N (QFN32, Tuya variant) per the LibreTiny board
# definition.
DEFAULT_PINS = [0, 1, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 20, 21, 22, 23, 24, 26, 28]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RN8209Scan),
        cv.Optional(CONF_PINS, default=DEFAULT_PINS): cv.ensure_list(
            cv.int_range(min=0, max=39)
        ),
        cv.Optional(CONF_CLOCK_DELAY_US, default=5): cv.int_range(min=1, max=500),
        cv.Optional(CONF_EXPECTED_ID, default=0x820900): cv.hex_int_range(
            min=0, max=0xFFFFFF
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for pin in config[CONF_PINS]:
        cg.add(var.add_pin(pin))
    cg.add(var.set_clock_delay(config[CONF_CLOCK_DELAY_US]))
    cg.add(var.set_expected_id(config[CONF_EXPECTED_ID]))
