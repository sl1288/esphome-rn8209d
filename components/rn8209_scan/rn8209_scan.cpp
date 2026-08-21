#include "rn8209_scan.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace rn8209_scan {

static const char *const TAG = "rn8209_scan";
static const size_t MAX_PINS = 40;

void RN8209Scan::setup() {
  this->idle_all_();
  const size_t n = this->pins_.size();
  this->total_ = n < 3 ? 0 : (uint32_t) (n * (n - 1) * (n - 2) * 2);
  ESP_LOGI(TAG, "Ready: %u pins, %u combinations. Start it from the button in the web interface.",
           (unsigned) n, (unsigned) this->total_);
}

void RN8209Scan::dump_config() {
  ESP_LOGCONFIG(TAG, "RN8209 SPI pin scanner:");
  ESP_LOGCONFIG(TAG, "  Expected device ID: 0x%06X", (unsigned) this->expected_);
  ESP_LOGCONFIG(TAG, "  Half clock period: %u us", (unsigned) this->clock_delay_);
  ESP_LOGCONFIG(TAG, "  Combinations: %u", (unsigned) this->total_);
}

void RN8209Scan::start() {
  if (this->running_) {
    ESP_LOGW(TAG, "Scan already running (%u/%u)", (unsigned) this->tested_, (unsigned) this->total_);
    return;
  }
  if (this->pins_.size() < 3) {
    ESP_LOGE(TAG, "At least 3 pins required");
    return;
  }
  this->i_ = 0;
  this->j_ = 0;
  this->k_ = 0;
  this->phase_ = 0;
  this->tested_ = 0;
  this->next_report_ = 0;
  this->hits_.clear();
  this->running_ = true;
  ESP_LOGI(TAG, "=== SPI scan started: %u combinations, looking for device ID 0x%06X ===",
           (unsigned) this->total_, (unsigned) this->expected_);
}

void RN8209Scan::stop() {
  if (!this->running_)
    return;
  this->running_ = false;
  this->idle_all_();
  ESP_LOGW(TAG, "Scan aborted at %u/%u", (unsigned) this->tested_, (unsigned) this->total_);
}

void RN8209Scan::idle_all_() {
  for (uint8_t pin : this->pins_)
    pinMode(pin, INPUT);
}

bool RN8209Scan::advance_() {
  const size_t n = this->pins_.size();
  do {
    this->k_++;
    if (this->k_ >= n) {
      this->k_ = 0;
      this->j_++;
    }
    if (this->j_ >= n) {
      this->j_ = 0;
      this->i_++;
    }
    if (this->i_ >= n) {
      this->i_ = 0;
      this->phase_++;
    }
    if (this->phase_ > 1)
      return false;
  } while (this->i_ == this->j_ || this->i_ == this->k_ || this->j_ == this->k_);
  return true;
}

void RN8209Scan::read_frame_(uint8_t sclk, uint8_t sdi, uint8_t cs, uint8_t phase, uint8_t cmd, uint32_t *acc) {
  const uint16_t d = this->clock_delay_;
  const size_t n = this->pins_.size();

  for (size_t idx = 0; idx < n; idx++)
    acc[idx] = 0;

  // Float every pin, then drive only the three outputs.
  for (size_t idx = 0; idx < n; idx++)
    pinMode(this->pins_[idx], INPUT);
  pinMode(sclk, OUTPUT);
  digitalWrite(sclk, LOW);
  pinMode(sdi, OUTPUT);
  digitalWrite(sdi, LOW);
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  delayMicroseconds(d * 4);

  // A falling chip-select edge starts the frame.
  digitalWrite(cs, LOW);
  delayMicroseconds(d * 2);

  // Command byte: bit 7 clear selects a read, bits 6..0 are the register
  // address. MSB first.
  for (int8_t bit = 7; bit >= 0; bit--) {
    digitalWrite(sdi, (cmd >> bit) & 0x01);
    delayMicroseconds(d);
    digitalWrite(sclk, HIGH);
    delayMicroseconds(d);
    digitalWrite(sclk, LOW);
  }
  delayMicroseconds(d * 2);

  // Read 24 data bits. The chip drives SDO on the rising edge and the value is
  // latched on the falling one (phase 0). Phase 1 samples half a clock period
  // earlier, in case the timing sits differently.
  for (uint8_t bit = 0; bit < 24; bit++) {
    digitalWrite(sclk, HIGH);
    delayMicroseconds(d);
    if (phase == 1) {
      for (size_t idx = 0; idx < n; idx++) {
        const uint8_t pin = this->pins_[idx];
        if (pin == sclk || pin == sdi || pin == cs)
          continue;
        acc[idx] = (acc[idx] << 1) | (uint32_t) (digitalRead(pin) ? 1 : 0);
      }
    }
    digitalWrite(sclk, LOW);
    delayMicroseconds(d);
    if (phase == 0) {
      for (size_t idx = 0; idx < n; idx++) {
        const uint8_t pin = this->pins_[idx];
        if (pin == sclk || pin == sdi || pin == cs)
          continue;
        acc[idx] = (acc[idx] << 1) | (uint32_t) (digitalRead(pin) ? 1 : 0);
      }
    }
  }

  // A rising chip-select edge ends the frame; float everything afterwards.
  digitalWrite(cs, HIGH);
  delayMicroseconds(d * 2);
  this->idle_all_();
}

void RN8209Scan::probe_(uint8_t sclk, uint8_t sdi, uint8_t cs, uint8_t phase) {
  uint32_t acc[MAX_PINS];
  this->read_frame_(sclk, sdi, cs, phase, 0x7F, acc);

  for (size_t idx = 0; idx < this->pins_.size(); idx++) {
    const uint8_t sdo = this->pins_[idx];
    if (sdo == sclk || sdo == sdi || sdo == cs)
      continue;
    if (acc[idx] != this->expected_)
      continue;

    // Cross-check: a real chip answers reproducibly.
    bool confirmed = true;
    for (uint8_t attempt = 0; attempt < 3 && confirmed; attempt++) {
      uint32_t verify[MAX_PINS];
      this->read_frame_(sclk, sdi, cs, phase, 0x7F, verify);
      if (verify[idx] != this->expected_)
        confirmed = false;
    }
    if (!confirmed) {
      ESP_LOGD(TAG, "Not reproducible: SCLK=P%u SDI=P%u SDO=P%u CS=P%u", sclk, sdi, sdo, cs);
      continue;
    }

    ESP_LOGW(TAG, "*** HIT *** SCLK=P%u  SDI=P%u  SDO=P%u  CS=P%u  (phase %u)  device ID=0x%06X", sclk, sdi,
             sdo, cs, phase, (unsigned) acc[idx]);
    this->hits_.push_back(ScanHit{sclk, sdi, sdo, cs, phase, acc[idx]});
  }
}

void RN8209Scan::loop() {
  if (!this->running_)
    return;

  const uint32_t begin = millis();
  while (millis() - begin < 20) {
    if (!this->advance_()) {
      this->running_ = false;
      this->idle_all_();
      this->report_();
      return;
    }
    this->probe_(this->pins_[this->i_], this->pins_[this->j_], this->pins_[this->k_], this->phase_);
    this->tested_++;
    App.feed_wdt();
  }

  if (this->tested_ >= this->next_report_) {
    ESP_LOGI(TAG, "Progress: %u/%u, %u hits", (unsigned) this->tested_, (unsigned) this->total_,
             (unsigned) this->hits_.size());
    this->next_report_ = this->tested_ + (this->total_ / 20) + 1;
  }
}

void RN8209Scan::report_() {
  ESP_LOGI(TAG, "=== Scan finished: %u combinations tested, %u hits ===", (unsigned) this->tested_,
           (unsigned) this->hits_.size());

  if (this->hits_.empty()) {
    ESP_LOGW(TAG, "No RN8209 found over SPI. Possible causes:");
    ESP_LOGW(TAG, "  - chips in UART mode (IS pin = 0) rather than SPI");
    ESP_LOGW(TAG, "  - metering handled by a separate MCU (TuyaMCU) on the UART");
    ESP_LOGW(TAG, "  - clock too fast: raise clock_delay_us and scan again");
    return;
  }

  // Group by bus: identical SCLK/SDI/SDO means one bus, the CS pins are the
  // individual chips on it.
  for (size_t a = 0; a < this->hits_.size(); a++) {
    bool already_listed = false;
    for (size_t b = 0; b < a; b++) {
      if (this->hits_[b].sclk == this->hits_[a].sclk && this->hits_[b].sdi == this->hits_[a].sdi &&
          this->hits_[b].sdo == this->hits_[a].sdo) {
        already_listed = true;
        break;
      }
    }
    if (already_listed)
      continue;

    ESP_LOGI(TAG, "Bus: SCLK=P%u  MOSI/SDI=P%u  MISO/SDO=P%u", this->hits_[a].sclk, this->hits_[a].sdi,
             this->hits_[a].sdo);
    for (size_t b = 0; b < this->hits_.size(); b++) {
      if (this->hits_[b].sclk != this->hits_[a].sclk || this->hits_[b].sdi != this->hits_[a].sdi ||
          this->hits_[b].sdo != this->hits_[a].sdo)
        continue;
      ESP_LOGI(TAG, "    Chip at CS=P%u (phase %u)", this->hits_[b].cs, this->hits_[b].phase);
    }
  }
}

}  // namespace rn8209_scan
}  // namespace esphome
