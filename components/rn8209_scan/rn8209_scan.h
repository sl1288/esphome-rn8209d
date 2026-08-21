#pragma once

#include "esphome/core/component.h"
#include <vector>

namespace esphome {
namespace rn8209_scan {

struct ScanHit {
  uint8_t sclk;
  uint8_t sdi;
  uint8_t sdo;
  uint8_t cs;
  uint8_t phase;
  uint32_t value;
};

class RN8209Scan : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void add_pin(uint8_t pin) { this->pins_.push_back(pin); }
  void set_clock_delay(uint16_t us) { this->clock_delay_ = us; }
  void set_expected_id(uint32_t value) { this->expected_ = value; }

  /// Start the scan (callable from a template button).
  void start();
  /// Abort a running scan.
  void stop();

 protected:
  /// Advance the odometer (phase, sclk, sdi, cs) to the next valid
  /// combination; false once the search space is exhausted.
  bool advance_();
  /// One read attempt including verification and logging of hits.
  void probe_(uint8_t sclk, uint8_t sdi, uint8_t cs, uint8_t phase);
  /// Clock out one SPI read frame and store the 24 bits per pin in acc[].
  void read_frame_(uint8_t sclk, uint8_t sdi, uint8_t cs, uint8_t phase, uint8_t cmd, uint32_t *acc);
  void idle_all_();
  void report_();

  std::vector<uint8_t> pins_;
  std::vector<ScanHit> hits_;
  uint16_t clock_delay_{5};
  uint32_t expected_{0x820900};

  bool running_{false};
  size_t i_{0};
  size_t j_{0};
  size_t k_{0};
  uint8_t phase_{0};
  uint32_t tested_{0};
  uint32_t total_{0};
  uint32_t next_report_{0};
};

}  // namespace rn8209_scan
}  // namespace esphome
