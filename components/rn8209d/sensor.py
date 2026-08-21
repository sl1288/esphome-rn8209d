"""Sensor platform for the RN8209C/D.

One entry corresponds to one chip. Several chips on the same bus are told apart
by their cs_pin.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import sensor
from esphome.const import (
    CONF_CS_PIN,
    CONF_FREQUENCY,
    CONF_ID,
    CONF_VOLTAGE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_HERTZ,
    UNIT_VOLT,
    UNIT_WATT,
)

from . import RN8209DBus, rn8209d_ns

DEPENDENCIES = ["rn8209d"]

RN8209D = rn8209d_ns.class_("RN8209D", cg.PollingComponent)

CONF_RN8209D_ID = "rn8209d_id"
CONF_CURRENT_A = "current_a"
CONF_CURRENT_B = "current_b"
CONF_POWER_A = "power_a"
CONF_POWER_B = "power_b"

# Raw value to reading: value = (raw - offset) * factor.
#
# Channels A and B are NOT equally sensitive (different pre-amplification, 16x
# against at most 4x), hence separate factors.
#
# The offsets exist for hardware that genuinely has a zero-point error. Do not
# reach for them to hide the noise floor of the RMS registers: noise adds to an
# RMS signal in quadrature rather than linearly, and the chip derives its power
# register from the raw values, so subtracting a noise floor makes current and
# power disagree. Use a threshold filter on the sensor instead.
CONF_VOLTAGE_FACTOR = "voltage_factor"
CONF_CURRENT_A_FACTOR = "current_a_factor"
CONF_CURRENT_B_FACTOR = "current_b_factor"
CONF_POWER_A_FACTOR = "power_a_factor"
CONF_POWER_B_FACTOR = "power_b_factor"
CONF_VOLTAGE_OFFSET = "voltage_offset"
CONF_CURRENT_A_OFFSET = "current_a_offset"
CONF_CURRENT_B_OFFSET = "current_b_offset"
CONF_CLOCK_FREQUENCY = "clock_frequency"

# Analog pre-amplification of the three ADC channels (SYSCON, register 0x00).
# Maximum input amplitude is 1000 mV peak - too much gain clips. The values are
# the steps the datasheet allows.
CONF_ENABLE_CHANNEL_B = "enable_channel_b"
CONF_PGA_VOLTAGE = "pga_voltage"
CONF_PGA_CURRENT_A = "pga_current_a"
CONF_PGA_CURRENT_B = "pga_current_b"

PGA_CURRENT_A = {1: 0b00, 2: 0b01, 8: 0b10, 16: 0b11}
PGA_CURRENT_B = {1: 0b00, 2: 0b01, 4: 0b10}
PGA_VOLTAGE = {1: 0b00, 2: 0b01, 4: 0b10}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RN8209D),
        cv.GenerateID(CONF_RN8209D_ID): cv.use_id(RN8209DBus),
        cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_schema,
        cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_FREQUENCY): sensor.sensor_schema(
            unit_of_measurement=UNIT_HERTZ,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_FREQUENCY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT_A): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT_B): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER_A): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER_B): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_FACTOR, default=1.0): cv.float_,
        cv.Optional(CONF_CURRENT_A_FACTOR, default=1.0): cv.float_,
        cv.Optional(CONF_CURRENT_B_FACTOR, default=1.0): cv.float_,
        cv.Optional(CONF_POWER_A_FACTOR, default=1.0): cv.float_,
        cv.Optional(CONF_POWER_B_FACTOR, default=1.0): cv.float_,
        cv.Optional(CONF_VOLTAGE_OFFSET, default=0.0): cv.float_,
        cv.Optional(CONF_CURRENT_A_OFFSET, default=0.0): cv.float_,
        cv.Optional(CONF_CURRENT_B_OFFSET, default=0.0): cv.float_,
        cv.Optional(CONF_CLOCK_FREQUENCY, default=3579545): cv.positive_int,
        cv.Optional(CONF_ENABLE_CHANNEL_B, default=True): cv.boolean,
        cv.Optional(CONF_PGA_VOLTAGE, default=1): cv.one_of(*PGA_VOLTAGE, int=True),
        cv.Optional(CONF_PGA_CURRENT_A, default=16): cv.one_of(
            *PGA_CURRENT_A, int=True
        ),
        cv.Optional(CONF_PGA_CURRENT_B, default=4): cv.one_of(
            *PGA_CURRENT_B, int=True
        ),
    }
).extend(cv.polling_component_schema("10s"))

SENSORS = {
    CONF_VOLTAGE: "set_voltage_sensor",
    CONF_FREQUENCY: "set_frequency_sensor",
    CONF_CURRENT_A: "set_current_a_sensor",
    CONF_CURRENT_B: "set_current_b_sensor",
    CONF_POWER_A: "set_power_a_sensor",
    CONF_POWER_B: "set_power_b_sensor",
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_RN8209D_ID])
    cg.add(var.set_bus(parent))

    cs = await cg.gpio_pin_expression(config[CONF_CS_PIN])
    cg.add(var.set_cs_pin(cs))

    for key, setter in SENSORS.items():
        if key not in config:
            continue
        sens = await sensor.new_sensor(config[key])
        cg.add(getattr(var, setter)(sens))

    cg.add(var.set_voltage_factor(config[CONF_VOLTAGE_FACTOR]))
    cg.add(var.set_current_a_factor(config[CONF_CURRENT_A_FACTOR]))
    cg.add(var.set_current_b_factor(config[CONF_CURRENT_B_FACTOR]))
    cg.add(var.set_power_a_factor(config[CONF_POWER_A_FACTOR]))
    cg.add(var.set_power_b_factor(config[CONF_POWER_B_FACTOR]))
    cg.add(var.set_voltage_offset(config[CONF_VOLTAGE_OFFSET]))
    cg.add(var.set_current_a_offset(config[CONF_CURRENT_A_OFFSET]))
    cg.add(var.set_current_b_offset(config[CONF_CURRENT_B_OFFSET]))
    cg.add(var.set_clock_frequency(config[CONF_CLOCK_FREQUENCY]))

    # SYSCON (0x00): bit 6 = ADC2ON, bits 5-4 = PGAIB, bits 3-2 = PGAU,
    # bits 1-0 = PGAIA. Bits 14-8 are read-only zero in SPI mode.
    syscon = (
        (0x40 if config[CONF_ENABLE_CHANNEL_B] else 0x00)
        | (PGA_CURRENT_B[config[CONF_PGA_CURRENT_B]] << 4)
        | (PGA_VOLTAGE[config[CONF_PGA_VOLTAGE]] << 2)
        | PGA_CURRENT_A[config[CONF_PGA_CURRENT_A]]
    )
    cg.add(var.set_syscon(syscon))
