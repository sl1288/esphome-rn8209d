#!/usr/bin/env python3
"""Extract the RN8209D metering calibration from a Voltcraft SEM8500 flash dump.

The stock Tuya firmware stores the factory calibration of its three RN8209D
metering chips as a plain-text JSON blob in the Tuya user-file area of the
flash, under the key ``coe_save_key``:

    {"0_V":998,"0_PA":646,"0_PB":171,"0_IA":132,"0_IB":35,
     "1_V":999,"1_PA":646,"1_PB":172,"1_IA":132,"1_IB":35,
     "2_V":998,"2_PA":651,"2_PB":172,"2_IA":133,"2_IB":35}

The blob sits *outside* the encrypted application image, so no decryption is
needed - a plain scan of the raw dump finds it. (If you also want to inspect
the application itself, decrypt it with ``bk7231tools dissect_dump -e``.)

The dump may hold many differing copies of the blob. They are not backups but
a write history: the factory calibration stores each step as it goes, one chip
and channel at a time, each into a fresh 4 KiB block, starting from a template
that is identical on every unit. Only one copy is current. An index sector in
front of the data blocks names it: after the key (a 32-byte, NUL-padded field)
follow the blob length and the block number, both as little-endian uint32,
counted in 4 KiB blocks from the start of that index sector. A second index
sector directly behind it is a backup copy. Taking the first blob found instead
yields the uncalibrated template - recognisable by chips 1 and 2 reading
exactly V=997, IA=133, IB=35.

Each chip measures one shared voltage and two independent current channels,
A and B, so three chips cover the six sockets.

Conversion from raw register values to physical units, verified against a
reference meter on a 60 W incandescent lamp:

    voltage [V] = raw / (10 * V)
    current [A] = raw / (1000 * I)
    power   [W] = raw * 32768 * voltage_factor * current_factor

The power scaling does NOT follow from the PA/PB coefficients. The chip forms
its power register internally as ``raw_P = raw_U * raw_I / 2**15``, so the
power factor is derived from the two verified factors instead. What the stock
firmware uses PA/PB for is unknown; most likely the energy pulse constant.

Usage:
    python3 extract_sem8500_calibration.py FLASH_DUMP.bin
    python3 extract_sem8500_calibration.py FLASH_DUMP.bin --yaml
    python3 extract_sem8500_calibration.py FLASH_DUMP.bin --sensors

``--yaml`` prints the ``csN_*`` substitutions consumed by the SEM8500 package
(``packages/sem8500.yaml`` in https://github.com/sl1288/esphome-packages);
paste them under ``substitutions:`` of your device file. ``--sensors`` prints
complete ``rn8209d`` sensor blocks for configs that do not use the package.

Obtain a dump with BK7231Flasher or ltchiptool before overwriting the stock
firmware. Keep it - without it you have to calibrate against a reference meter
by hand.
"""

from __future__ import annotations

import argparse
import json
import re
import sys

# Chip-select pin and the two sockets each chip measures (channel A, channel B).
#
# CAVEAT: which coefficient index (0/1/2) belongs to which chip-select pin is
# NOT known - the stock firmware's chip order is not observable from outside.
# The assignment below is the one used by the published device page, chosen
# arbitrarily. Since the three coefficient sets differ by well under 1 %
# (V: 998/999/998, IA: 132/132/133), a wrong assignment is not worth chasing.
CHIPS = [
    ("P9", "Socket 3", "Socket 4"),
    ("P15", "Socket 5", "Socket 6"),
    ("P20", "Socket 1", "Socket 2"),
]

FACTOR_KEYS = (
    "voltage_factor",
    "current_a_factor",
    "current_b_factor",
    "power_a_factor",
    "power_b_factor",
)

# Internal relation of the RN8209D: raw_P = raw_U * raw_I / 2**15
POWER_SHIFT = 32768

BLOB_PATTERN = re.compile(rb'\{"[0-9]_V":[0-9]+(?:,"[0-9]_[A-Z]{1,2}":[0-9]+)+\}')

# Index entry of the Tuya key-value store, see the module docstring.
INDEX_KEY = b"coe_save_key"
INDEX_KEY_FIELD = 32
BLOCK_SIZE = 0x1000


def find_blobs(data: bytes) -> list[tuple[int, dict[str, int]]]:
    """Return every calibration blob found, as (offset, parsed dict)."""
    results: list[tuple[int, dict[str, int]]] = []
    for match in BLOB_PATTERN.finditer(data):
        try:
            parsed = json.loads(match.group().decode("ascii"))
        except (ValueError, UnicodeDecodeError):
            continue
        if not isinstance(parsed, dict):
            continue
        results.append((match.start(), {k: int(v) for k, v in parsed.items()}))
    return results


def indexed_offset(data: bytes, blobs: list[tuple[int, dict[str, int]]]) -> int | None:
    """Return the offset of the blob the key-value index marks as current.

    An index entry only counts if its length and block number point exactly at
    the start of a blob of that length, so a misread entry - or the backup
    index sector, whose block numbers are relative to the primary one - can
    never select a wrong blob.
    """
    lengths = {
        offset: len(BLOB_PATTERN.match(data, offset).group()) for offset, _ in blobs
    }
    key_field = INDEX_KEY.ljust(INDEX_KEY_FIELD, b"\0")
    position = data.find(key_field)
    while position != -1:
        entry = position + INDEX_KEY_FIELD
        length = int.from_bytes(data[entry : entry + 4], "little")
        block = int.from_bytes(data[entry + 4 : entry + 8], "little")
        target = (position & ~(BLOCK_SIZE - 1)) + block * BLOCK_SIZE
        if lengths.get(target) == length:
            return target
        position = data.find(key_field, position + 1)
    return None


def factors(coefficients: dict[str, int], index: int) -> dict[str, float] | None:
    """Compute the ESPHome scaling factors for one chip."""
    try:
        volt = coefficients[f"{index}_V"]
        cur_a = coefficients[f"{index}_IA"]
        cur_b = coefficients[f"{index}_IB"]
    except KeyError:
        return None
    if min(volt, cur_a, cur_b) <= 0:
        return None

    voltage_factor = 1.0 / (10.0 * volt)
    current_a_factor = 1.0 / (1000.0 * cur_a)
    current_b_factor = 1.0 / (1000.0 * cur_b)
    return {
        "voltage_factor": voltage_factor,
        "current_a_factor": current_a_factor,
        "current_b_factor": current_b_factor,
        "power_a_factor": POWER_SHIFT * voltage_factor * current_a_factor,
        "power_b_factor": POWER_SHIFT * voltage_factor * current_b_factor,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract RN8209D calibration from a SEM8500 flash dump."
    )
    parser.add_argument("dump", help="raw flash dump (2 MiB, e.g. from BK7231Flasher)")
    output = parser.add_mutually_exclusive_group()
    output.add_argument(
        "--yaml",
        action="store_true",
        help="print ready-to-paste substitutions for the SEM8500 package",
    )
    output.add_argument(
        "--sensors",
        action="store_true",
        help="print complete rn8209d sensor blocks (for configs without the package)",
    )
    args = parser.parse_args()
    table = not (args.yaml or args.sensors)

    try:
        with open(args.dump, "rb") as handle:
            data = handle.read()
    except OSError as error:
        print(f"Cannot read {args.dump}: {error}", file=sys.stderr)
        return 2

    blobs = find_blobs(data)
    if not blobs:
        print(
            "No calibration blob found.\n"
            "  - Is this a full flash dump of a SEM8500 with stock firmware?\n"
            "  - A dump taken after flashing ESPHome no longer contains it.\n"
            "  - Try searching manually: strings -n 8 dump.bin | grep '_IA'",
            file=sys.stderr,
        )
        return 1

    # Several differing copies are normal: they are the calibration history.
    offset = indexed_offset(data, blobs)
    if offset is None:
        # No usable index: fall back to the highest offset, which is the
        # newest copy as long as the store has not wrapped around.
        offset = blobs[-1][0]
        if len({json.dumps(b, sort_keys=True) for _, b in blobs}) > 1:
            print(
                f"Warning: {len(blobs)} differing blobs and no usable "
                f"{INDEX_KEY.decode()} index entry found, using the last one at "
                f"0x{offset:06X}. Inspect the others manually.",
                file=sys.stderr,
            )
    coefficients = dict(blobs)[offset]

    if table:
        print(f"Found calibration at flash offset 0x{offset:06X}")
        if len(blobs) > 1:
            print(f"({len(blobs)} copies in total - older ones are calibration history)")
        print()
        print("Raw coefficients:")
        print(json.dumps(coefficients, sort_keys=True))
        print()

    for index, (cs_pin, socket_a, socket_b) in enumerate(CHIPS):
        computed = factors(coefficients, index)
        if computed is None:
            print(f"Chip {index}: coefficients incomplete, skipped", file=sys.stderr)
            continue

        if args.yaml:
            if index == 0:
                print("  # Calibration of THIS unit, read from its flash dump with")
                print("  # tools/extract_sem8500_calibration.py.")
            else:
                print()
            prefix = "cs" + cs_pin.lstrip("P")
            for key in FACTOR_KEYS:
                print(f'  {prefix}_{key}: "{computed[key]:.10f}"')
        elif args.sensors:
            print(f"  # Coefficient set {index} -> {socket_a} (A) and {socket_b} (B)")
            print("  - platform: rn8209d")
            print(f"    cs_pin: {cs_pin}")
            print("    update_interval: 2s")
            for key in FACTOR_KEYS:
                print(f"    {key}: {computed[key]:.10f}")
            print()
        else:
            print(f"Coefficient set {index}  (chip on CS = {cs_pin})")
            print(
                f"  V={coefficients[f'{index}_V']:4d}"
                f"  IA={coefficients[f'{index}_IA']:4d}"
                f"  IB={coefficients[f'{index}_IB']:4d}"
                f"  PA={coefficients.get(f'{index}_PA', 0):4d}"
                f"  PB={coefficients.get(f'{index}_PB', 0):4d}"
            )
            for key, value in computed.items():
                print(f"    {key:18s} {value:.10f}")
            print()

    if table:
        print("Re-run with --yaml to get pasteable substitutions for the package,")
        print("or with --sensors to get complete rn8209d sensor blocks.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
