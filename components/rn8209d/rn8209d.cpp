#include "rn8209d.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rn8209d {

static const char *const TAG = "rn8209d";

// ---------------------------------------------------------------- Bus

void RN8209DBus::setup() {
  this->clk_->setup();
  this->clk_->digital_write(false);
  this->mosi_->setup();
  this->mosi_->digital_write(false);
  this->miso_->setup();
}

void RN8209DBus::dump_config() {
  ESP_LOGCONFIG(TAG, "RN8209 software SPI bus:");
  LOG_PIN("  SCLK Pin: ", this->clk_);
  LOG_PIN("  MOSI Pin: ", this->mosi_);
  LOG_PIN("  MISO Pin: ", this->miso_);
  ESP_LOGCONFIG(TAG, "  Half clock period: %u us", (unsigned) this->clock_delay_);
}

void RN8209DBus::shift_out_(uint8_t data) {
  const uint16_t d = this->clock_delay_;
  for (int8_t bit = 7; bit >= 0; bit--) {
    this->mosi_->digital_write((data >> bit) & 0x01);
    delayMicroseconds(d);
    this->clk_->digital_write(true);
    delayMicroseconds(d);
    this->clk_->digital_write(false);
  }
}

uint32_t RN8209DBus::shift_in_(uint8_t length) {
  const uint16_t d = this->clock_delay_;
  uint32_t value = 0;
  const uint8_t bits = length * 8;
  for (uint8_t i = 0; i < bits; i++) {
    this->clk_->digital_write(true);
    delayMicroseconds(d);
    this->clk_->digital_write(false);
    delayMicroseconds(d);
    value = (value << 1) | (uint32_t) (this->miso_->digital_read() ? 1 : 0);
  }
  return value;
}

uint32_t RN8209DBus::read_register(GPIOPin *cs, uint8_t reg, uint8_t length) {
  const uint16_t d = this->clock_delay_;

  cs->digital_write(false);  // falling edge starts the frame
  delayMicroseconds(d * 2);
  // Command byte: bit 7 clear selects a read, bits 6..0 are the address.
  this->shift_out_(reg & 0x7F);
  delayMicroseconds(d * 2);
  const uint32_t value = this->shift_in_(length);
  cs->digital_write(true);
  delayMicroseconds(d);
  return value;
}

void RN8209DBus::write_register(GPIOPin *cs, uint8_t reg, uint8_t length, uint32_t value) {
  const uint16_t d = this->clock_delay_;

  cs->digital_write(false);
  delayMicroseconds(d * 2);
  // Command byte: bit 7 set selects a write.
  this->shift_out_(0x80 | (reg & 0x7F));
  delayMicroseconds(d * 2);
  for (int8_t i = length - 1; i >= 0; i--)
    this->shift_out_((uint8_t) (value >> (i * 8)));
  cs->digital_write(true);
  delayMicroseconds(d);
}

void RN8209DBus::special_command(GPIOPin *cs, uint8_t data) {
  const uint16_t d = this->clock_delay_;

  cs->digital_write(false);
  delayMicroseconds(d * 2);
  this->shift_out_(CMD_SPECIAL);
  delayMicroseconds(d * 2);
  this->shift_out_(data);
  cs->digital_write(true);
  delayMicroseconds(d);
}

// ---------------------------------------------------------------- Chip

void RN8209D::setup() {
  this->cs_->setup();
  this->cs_->digital_write(true);  // active low, so idle high
  delayMicroseconds(50);

  const uint32_t id = this->bus_->read_register(this->cs_, REG_DEVICE_ID, 3);
  if (id != RN8209D_DEVICE_ID) {
    ESP_LOGE(TAG, "No RN8209 found: device ID 0x%06X instead of 0x%06X", (unsigned) id,
             (unsigned) RN8209D_DEVICE_ID);
    this->mark_failed();
    return;
  }

  // SYSCON holds the ADC gains and the ADC2ON bit that enables the current
  // channel B ADC - while it is clear, channel B reads a constant zero. The
  // register sits in the write-protected range 0x00-0x17.
  this->bus_->special_command(this->cs_, CMD_WRITE_ENABLE);
  this->bus_->write_register(this->cs_, REG_SYSCON, 2, this->syscon_);
  this->bus_->special_command(this->cs_, CMD_WRITE_PROTECT);

  const uint32_t readback = this->bus_->read_register(this->cs_, REG_SYSCON, 2);
  if ((readback & 0x00FF) != (this->syscon_ & 0x00FF)) {
    ESP_LOGW(TAG, "SYSCON not accepted: read 0x%04X, wrote 0x%04X", (unsigned) readback,
             (unsigned) this->syscon_);
  } else {
    ESP_LOGI(TAG, "RN8209 detected, device ID 0x%06X, SYSCON 0x%04X", (unsigned) id, (unsigned) readback);
  }

  // Diagnostics: SysStatus bit 2 mirrors the IS pin (1 = chip is in SPI mode).
  // EMUStatus carries the checksum of the calibration registers 0x00-0x17;
  // the default in SPI mode is 0xEE79.
  const uint32_t sys_status = this->bus_->read_register(this->cs_, REG_SYS_STATUS, 1);
  const uint32_t emu_status = this->bus_->read_register(this->cs_, REG_EMU_STATUS, 3);
  ESP_LOGI(TAG, "SysStatus 0x%02X (interface: %s), EMUStatus 0x%06X (calibration checksum 0x%04X)",
           (unsigned) sys_status, (sys_status & 0x04) ? "SPI" : "UART", (unsigned) emu_status,
           (unsigned) (emu_status & 0xFFFF));
}

void RN8209D::dump_config() {
  ESP_LOGCONFIG(TAG, "RN8209C/D:");
  LOG_PIN("  CS Pin: ", this->cs_);
  ESP_LOGCONFIG(TAG, "  SYSCON: 0x%04X (channel B ADC %s)", (unsigned) this->syscon_,
                (this->syscon_ & 0x40) ? "on" : "off");
  ESP_LOGCONFIG(TAG, "  Factors: U=%.6f  IA=%.6f  IB=%.6f  PA=%.6f  PB=%.6f", this->voltage_factor_,
                this->current_a_factor_, this->current_b_factor_, this->power_a_factor_, this->power_b_factor_);
  ESP_LOGCONFIG(TAG, "  Offsets: U=%.1f  IA=%.1f  IB=%.1f", this->voltage_offset_, this->current_a_offset_,
                this->current_b_offset_);
  ESP_LOGCONFIG(TAG, "  Chip clock: %u Hz", (unsigned) this->clock_frequency_);
  LOG_UPDATE_INTERVAL(this);
  if (this->is_failed())
    ESP_LOGE(TAG, "  Communication failed!");
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_);
  LOG_SENSOR("  ", "Frequency", this->frequency_sensor_);
  LOG_SENSOR("  ", "Current A", this->current_a_sensor_);
  LOG_SENSOR("  ", "Current B", this->current_b_sensor_);
  LOG_SENSOR("  ", "Power A", this->power_a_sensor_);
  LOG_SENSOR("  ", "Power B", this->power_b_sensor_);
}

float RN8209D::read_rms_(uint8_t reg, float offset, float factor) {
  const uint32_t raw = this->bus_->read_register(this->cs_, reg, 3);
  // 24-bit value: a set MSB marks the reading invalid, to be treated as zero.
  if (raw & 0x800000)
    return 0.0f;
  const float corrected = (float) raw - offset;
  return corrected <= 0.0f ? 0.0f : corrected * factor;
}

float RN8209D::read_power_(uint8_t reg, float factor) {
  const uint32_t raw = this->bus_->read_register(this->cs_, reg, 4);
  return (float) ((int32_t) raw) * factor;
}

void RN8209D::update() {
  if (this->is_failed())
    return;

  if (this->voltage_sensor_ != nullptr)
    this->voltage_sensor_->publish_state(
        this->read_rms_(REG_U_RMS, this->voltage_offset_, this->voltage_factor_));

  if (this->current_a_sensor_ != nullptr)
    this->current_a_sensor_->publish_state(
        this->read_rms_(REG_IA_RMS, this->current_a_offset_, this->current_a_factor_));

  if (this->current_b_sensor_ != nullptr)
    this->current_b_sensor_->publish_state(
        this->read_rms_(REG_IB_RMS, this->current_b_offset_, this->current_b_factor_));

  if (this->power_a_sensor_ != nullptr)
    this->power_a_sensor_->publish_state(this->read_power_(REG_POWER_PA, this->power_a_factor_));

  if (this->power_b_sensor_ != nullptr)
    this->power_b_sensor_->publish_state(this->read_power_(REG_POWER_PB, this->power_b_factor_));

  if (this->frequency_sensor_ != nullptr) {
    // Datasheet section 2.9: f = CLKIN / 8 / UFREQ
    const uint32_t raw = this->bus_->read_register(this->cs_, REG_U_FREQ, 2);
    const float frequency = raw == 0 ? NAN : (float) this->clock_frequency_ / 8.0f / (float) raw;
    this->frequency_sensor_->publish_state(frequency);
  }
}

}  // namespace rn8209d
}  // namespace esphome
