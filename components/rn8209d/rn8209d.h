#pragma once

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace rn8209d {

/// Register addresses, datasheet Rev 3.5 section 2.12.1.
static const uint8_t REG_SYSCON = 0x00;      // 2 bytes, system control (ADC gains, ADC2ON)
static const uint8_t REG_IA_RMS = 0x22;      // 3 bytes, current channel A
static const uint8_t REG_IB_RMS = 0x23;      // 3 bytes, current channel B
static const uint8_t REG_U_RMS = 0x24;       // 3 bytes, voltage
static const uint8_t REG_U_FREQ = 0x25;      // 2 bytes, line frequency
static const uint8_t REG_POWER_PA = 0x26;    // 4 bytes, active power A (signed)
static const uint8_t REG_POWER_PB = 0x27;    // 4 bytes, active power B (signed)
static const uint8_t REG_EMU_STATUS = 0x2D;  // 3 bytes, includes calibration checksum
static const uint8_t REG_SYS_STATUS = 0x43;  // 1 byte, bit 2 = IS (1 = SPI mode)
static const uint8_t REG_DEVICE_ID = 0x7F;   // 3 bytes, fixed identifier 0x820900

static const uint32_t RN8209D_DEVICE_ID = 0x820900;

/// Special commands: command byte 0xEA followed by one data byte (datasheet
/// section 2.11.6). Registers 0x00-0x17 are write-protected and only writable
/// after WRITE_ENABLE.
static const uint8_t CMD_SPECIAL = 0xEA;
static const uint8_t CMD_WRITE_ENABLE = 0xE5;
static const uint8_t CMD_WRITE_PROTECT = 0xDC;
static const uint8_t CMD_RESET = 0xFA;

/// Software SPI bus for one or more RN8209 chips.
///
/// Timing per datasheet section 4.1.1: the clock idles low, data is driven on
/// the rising edge and latched on the falling edge. Chip select is active low;
/// its falling edge starts a frame.
///
/// A bit-banged bus is required because LibreTiny does not implement Arduino
/// SPI for BK72xx and ESPHome's spi component excludes the platform.
class RN8209DBus : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  void set_clk_pin(InternalGPIOPin *pin) { this->clk_ = pin; }
  void set_mosi_pin(InternalGPIOPin *pin) { this->mosi_ = pin; }
  void set_miso_pin(InternalGPIOPin *pin) { this->miso_ = pin; }
  void set_clock_delay(uint16_t microseconds) { this->clock_delay_ = microseconds; }

  /// Read a register from the chip addressed by cs (1-4 bytes, MSB first).
  uint32_t read_register(GPIOPin *cs, uint8_t reg, uint8_t length);
  /// Write a register (the command byte has bit 7 set).
  void write_register(GPIOPin *cs, uint8_t reg, uint8_t length, uint32_t value);
  /// Send a special command (0xEA followed by a data byte).
  void special_command(GPIOPin *cs, uint8_t data);

 protected:
  void shift_out_(uint8_t data);
  uint32_t shift_in_(uint8_t length);

  InternalGPIOPin *clk_{nullptr};
  InternalGPIOPin *mosi_{nullptr};
  InternalGPIOPin *miso_{nullptr};
  uint16_t clock_delay_{4};
};

class RN8209D : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_bus(RN8209DBus *bus) { this->bus_ = bus; }
  void set_cs_pin(GPIOPin *pin) { this->cs_ = pin; }
  void set_syscon(uint16_t syscon) { this->syscon_ = syscon; }

  void set_voltage_sensor(sensor::Sensor *sensor) { this->voltage_sensor_ = sensor; }
  void set_frequency_sensor(sensor::Sensor *sensor) { this->frequency_sensor_ = sensor; }
  void set_current_a_sensor(sensor::Sensor *sensor) { this->current_a_sensor_ = sensor; }
  void set_current_b_sensor(sensor::Sensor *sensor) { this->current_b_sensor_ = sensor; }
  void set_power_a_sensor(sensor::Sensor *sensor) { this->power_a_sensor_ = sensor; }
  void set_power_b_sensor(sensor::Sensor *sensor) { this->power_b_sensor_ = sensor; }

  void set_voltage_factor(float factor) { this->voltage_factor_ = factor; }
  void set_current_a_factor(float factor) { this->current_a_factor_ = factor; }
  void set_current_b_factor(float factor) { this->current_b_factor_ = factor; }
  void set_power_a_factor(float factor) { this->power_a_factor_ = factor; }
  void set_power_b_factor(float factor) { this->power_b_factor_ = factor; }
  void set_voltage_offset(float offset) { this->voltage_offset_ = offset; }
  void set_current_a_offset(float offset) { this->current_a_offset_ = offset; }
  void set_current_b_offset(float offset) { this->current_b_offset_ = offset; }
  void set_clock_frequency(uint32_t hz) { this->clock_frequency_ = hz; }

 protected:
  /// RMS registers are 24 bit; a set MSB means "read as zero" per the
  /// datasheet. Result: (raw - offset) * factor, clamped at zero.
  float read_rms_(uint8_t reg, float offset, float factor);
  /// Power registers are 32 bit two's complement.
  float read_power_(uint8_t reg, float factor);

  RN8209DBus *bus_{nullptr};
  GPIOPin *cs_{nullptr};
  uint16_t syscon_{0x0043};

  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *frequency_sensor_{nullptr};
  sensor::Sensor *current_a_sensor_{nullptr};
  sensor::Sensor *current_b_sensor_{nullptr};
  sensor::Sensor *power_a_sensor_{nullptr};
  sensor::Sensor *power_b_sensor_{nullptr};

  float voltage_factor_{1.0f};
  float current_a_factor_{1.0f};
  float current_b_factor_{1.0f};
  float power_a_factor_{1.0f};
  float power_b_factor_{1.0f};
  float voltage_offset_{0.0f};
  float current_a_offset_{0.0f};
  float current_b_offset_{0.0f};
  uint32_t clock_frequency_{3579545};
};

}  // namespace rn8209d
}  // namespace esphome
