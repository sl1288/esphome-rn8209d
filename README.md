# esphome-rn8209d

ESPHome external component for the **Renergy RN8209C / RN8209D** single-phase
energy metering IC, connected over SPI.

One chip measures a shared voltage plus **two independent current channels**
(A and B), each with its own active power register — so a single chip covers two
outlets, and several chips share one bus with individual chip selects.

There is no built-in ESPHome support for this chip, and the only other
implementation I am aware of (OpenBeken's `drv_rn8209.c`) is a UART stub that
performs no initialisation.

## Supported platforms

Tested on **BK7231N** (LibreTiny / `bk72xx`). The bus is bit-banged in software,
so it works on any ESPHome platform with digital I/O.

A software bus is not a stylistic choice: LibreTiny does not implement Arduino
`SPI` for BK72xx, and ESPHome's `spi` component excludes the platform, so a
hardware SPI bus is simply not available there. Measured cost on a BK7231N is
about 3 ms per chip poll — roughly 1 % CPU with three chips at a 1 s interval,
and irrelevant next to the chip's own register refresh rate of 3.4 Hz.

## Installation

```yaml
external_components:
  - source: github://sl1288/esphome-rn8209d
    components: [rn8209d]
```

## Configuration

Declare the bus once, then one `sensor` entry per chip:

```yaml
rn8209d:
  id: meter_bus
  clk_pin: P14
  mosi_pin: P16
  miso_pin: P17

sensor:
  - platform: rn8209d
    cs_pin: P9
    update_interval: 1s
    voltage_factor: 0.0001002004
    current_a_factor: 0.0000075758
    current_b_factor: 0.0000285714
    power_a_factor: 0.0000248740
    power_b_factor: 0.0000938105
    voltage:
      name: "Voltage"
    frequency:
      name: "Frequency"
    current_a:
      name: "Outlet 1 Current"
    current_b:
      name: "Outlet 2 Current"
    power_a:
      name: "Outlet 1 Power"
    power_b:
      name: "Outlet 2 Power"
```

### Bus options (`rn8209d:`)

| Option | Default | Meaning |
| --- | --- | --- |
| `clk_pin` | required | SCLK |
| `mosi_pin` | required | SDI (host to chip) |
| `miso_pin` | required | SDO (chip to host) |
| `clock_delay_us` | `4` | Half clock period; ~125 kHz. Raise it if transfers are unreliable. |

### Chip options (`sensor:` platform `rn8209d`)

| Option | Default | Meaning |
| --- | --- | --- |
| `cs_pin` | required | Chip select for this chip (active low) |
| `voltage_factor` | `1.0` | Raw → volts |
| `current_a_factor`, `current_b_factor` | `1.0` | Raw → amperes, per channel |
| `power_a_factor`, `power_b_factor` | `1.0` | Raw → watts, per channel |
| `voltage_offset`, `current_a_offset`, `current_b_offset` | `0.0` | Subtracted from the raw value before scaling — see the warning below |
| `pga_current_a` | `16` | Analog gain, channel A: 1, 2, 8 or 16 |
| `pga_current_b` | `4` | Analog gain, channel B: 1, 2 or 4 |
| `pga_voltage` | `1` | Analog gain, voltage: 1, 2 or 4 |
| `enable_channel_b` | `true` | Sets `ADC2ON` — see below |
| `clock_frequency` | `3579545` | Chip clock in Hz, used for the frequency calculation |

Sensors: `voltage`, `frequency`, `current_a`, `current_b`, `power_a`, `power_b`.
All optional; only the registers you ask for are read.

Channels A and B are **not** equally sensitive — channel A reaches a gain of 16
while channel B stops at 4 — hence the separate factors for each.

## Things that will cost you an evening

**Current channel B is off by default.** Bit 6 (`ADC2ON`) of `SYSCON` gates the
second current ADC; while it is clear, `IBRMS` and `PowerPB` read exactly zero
and nothing hints at why. This component sets it during `setup()`. `SYSCON` is
inside the write-protected range `0x00`–`0x17`, so the write is wrapped in the
special commands `0xEA 0xE5` (write enable) and `0xEA 0xDC` (write protect), and
read back afterwards.

**Power does not scale from a "power coefficient".** The chip forms its power
register internally as `raw_P = raw_U * raw_I / 2^15`, so the correct factor is

```
power_factor = 32768 * voltage_factor * current_factor
```

Deriving it this way keeps power consistent with voltage × current while still
reporting *active* power from the chip's own register, which is what you want
for reactive loads.

**Do not subtract a current offset to hide the noise floor.** With no load the
RMS current registers read a few hundred counts. That is a noise floor, not a
zero-point error, and subtracting it makes things worse:

* noise adds to an RMS signal in quadrature, not linearly;
* the chip's power register is derived from the **raw** registers, so
  subtracting from current makes current and power disagree — verified at ~1 %
  on channel A and ~4 % on channel B;
* the decisive tell: with no load the current register shows a few milliamps
  while active power shows 0.0 W. A real offset in the current path would have
  to bring a corresponding wattage with it. It does not, because an RMS value
  rectifies noise into itself whereas active power is a signed average in which
  noise cancels.

If you want a clean zero, use a threshold filter on the sensor
(`- lambda: "return x < 0.3f ? 0.0f : x;"`) rather than an offset: that leaves
real loads untouched.

**Energy per channel has to be integrated in software.** The chip's hardware
energy counter can accumulate only one of its two channels at a time (special
commands `0xEA 0x5A` / `0xEA 0xA5` select which), so for two outlets per chip use
ESPHome's `total_daily_energy` on the power sensors.

**How you report to Home Assistant decides how accurate that energy is.**
`total_daily_energy` does not integrate on a clock of its own: it hooks the power
sensor's state callback and, with the default method `right`, multiplies each
published value by the time elapsed since the previous publication. So whatever
the power sensor publishes *is* the integration.

That makes a plain slow `update_interval` a poor way to reduce Home Assistant
traffic: a load switching between two samples is charged for the whole interval,
up to `P × interval` of error per transition — 8 Wh for a 2 kW load at 15 s.
Errors from switch-on and switch-off have opposite signs and largely cancel over
many cycles, but bursts shorter than the interval can be missed entirely.

Poll fast and average on the way out instead:

```yaml
    power_a:
      name: "Outlet 1 Power"
      filters:
        - throttle_average: 15s
```

`throttle_average` publishes the *mean* of the samples in the window, and mean
power × elapsed time is exactly the energy of the sampled signal — nothing
between samples is lost. Do **not** switch to `method: trapezoid` in this setup:
it would average two already-averaged values and blur the result.

One consequence worth knowing: anything that reads a sensor's `.state` in a
lambda now sees the throttled value. If you drive a protective action from it,
read `get_raw_state()` instead — that returns the value from the last poll,
before the filters, so the action keeps reacting at the polling rate. The example
config uses this for its overcurrent cutoff.

`total_daily_energy` keeps its counter in flash (`restore` defaults to true).
That costs nothing while nothing changes: `save()` only queues the value in RAM,
and the periodic `sync()` compares against the stored copy and skips unchanged
values -- so with the relay gate holding power at zero, an idle outlet writes
nothing at all. Under load the six counters do change, and the default
`flash_write_interval` of 60 s means one write per minute. The example raises it:

```yaml
preferences:
  flash_write_interval: 300s
```

That cuts the wear fivefold and risks up to five minutes of counter state on an
unclean power loss -- a reasonable trade for a board sealed inside a power strip.

There is little point pushing the poll below about a second. The registers
refresh at 3.4 Hz and `PowerPA`/`PowerPB` are averaged rather than instantaneous,
so sampling is unbiased and the residual error on a daily total stays well under
the chip's own ±1 % accuracy — which is what actually limits the result.

## Protocol notes

| Register | Bytes | Meaning |
| --- | --- | --- |
| `0x00` | 2 | `SYSCON` — ADC gains, `ADC2ON` |
| `0x22` | 3 | Current A RMS |
| `0x23` | 3 | Current B RMS |
| `0x24` | 3 | Voltage RMS |
| `0x25` | 2 | Line frequency, `f = CLKIN / 8 / raw` |
| `0x26` | 4 | Active power A, two's complement |
| `0x27` | 4 | Active power B, two's complement |
| `0x2D` | 3 | `EMUStatus`, low 16 bits = calibration checksum (`0xEE79` in SPI mode) |
| `0x43` | 1 | `SysStatus`, bit 2 = interface (1 = SPI) |
| `0x7F` | 3 | Device ID, always `0x820900` |

Chip select is active low. The command byte is `{R/W, addr[6:0]}`, bit 7 clear
for a read. Data is MSB first, changes on the rising clock edge and is sampled
on the falling one — SPI mode 1 (CPOL 0, CPHA 1). Reading an invalid address
returns `0x00`. RMS registers are 24 bit and a set MSB means "treat as zero".

Register widths vary per address, which is a common source of wrong readings —
note that `0x25` is 2 bytes and `0x26`–`0x28` are 4.

**The RN8209C only speaks UART**, not SPI, and is therefore not usable with this
component. The chip's `IS` pin selects the interface; `SysStatus` bit 2 lets you
read back which mode it is in.

Datasheet: RN8209C/RN8209D User Manual Rev 3.5.

## Finding the wiring: `rn8209_scan`

If you are reverse-engineering a device and do not know which GPIOs carry the
bus, the repo includes a scanner component. It tries every combination of
SCLK / SDI / CS from a pin list and reads the device ID register, sampling all
remaining pins simultaneously as SDO candidates — which removes one dimension
from the search and makes a 19-pin brute force finish in about a minute.

```yaml
external_components:
  - source: github://sl1288/esphome-rn8209d
    components: [rn8209_scan]

rn8209_scan:
  id: pin_scanner
  pins: [0, 1, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 20, 21, 22, 23, 24, 26, 28]

button:
  - platform: template
    name: "Start RN8209 scan"
    on_press:
      - lambda: "id(pin_scanner)->start();"
```

Hits are logged grouped by bus, listing the chip selects found on it. Every hit
is re-read three times, so a one-off glitch does not get reported.

**The scan drives every pin in the list.** On a mains device that includes the
relay pins. Unplug all loads first — which is why it only starts on a button
press, never automatically.

## Reference device: Voltcraft SEM8500

The component was written for the Voltcraft SEM8500 (Conrad 2359015), a
six-outlet metering power strip: BK7231N, six relays, three RN8209D on one SPI
bus.

See [`example-sem8500.yaml`](example-sem8500.yaml) for a complete configuration,
including per-outlet energy counters and a rebuilt overcurrent protection — the
stock protection lived in the Tuya firmware and is lost when reflashing.

Its stock firmware stores the factory calibration as plain-text JSON in the Tuya
user-file area of the flash, outside the encrypted application image.
[`tools/extract_sem8500_calibration.py`](tools/extract_sem8500_calibration.py)
reads it out of a flash dump and prints ready-to-paste factors:

```bash
python3 tools/extract_sem8500_calibration.py flash_dump.bin --yaml
```

Take a dump before overwriting the stock firmware. Without it you have to
calibrate against a reference meter by hand.

## License

MIT
