#!/usr/bin/env python3
"""Generate pinned Unicode 16 default casing from explicitly supplied source data."""

import argparse
import hashlib
from pathlib import Path

SOURCE_HASHES = {
    "UnicodeData.txt": "ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f",
    "SpecialCasing.txt": "8d5de354eef79f2395a54c9c7dcebbaf3d30fc962d0f85611ea97aa973a0c451",
    "DerivedCoreProperties.txt": "39d35161f2954497f69e08bdb9e701493f476a3d30222de20028feda36c1dabd",
}


def generate(source: Path, output: Path) -> None:
    for filename, expected in SOURCE_HASHES.items():
        actual = hashlib.sha256((source / filename).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"{filename}: expected pinned Unicode 16 SHA-256 {expected}")

    lower, upper = {}, {}
    for row in (source / "UnicodeData.txt").read_text().splitlines():
        fields = row.split(";")
        code = int(fields[0], 16)
        if fields[12]:
            upper[code] = tuple(int(item, 16) for item in fields[12].split())
        if fields[13]:
            lower[code] = tuple(int(item, 16) for item in fields[13].split())

    for row in (source / "SpecialCasing.txt").read_text().splitlines():
        row = row.split("#")[0].strip()
        if not row:
            continue
        fields = [item.strip() for item in row.split(";")]
        conditions = fields[4].split()
        if conditions:
            if conditions == ["Final_Sigma"]:
                if int(fields[0], 16) != 0x3A3 or fields[1] != "03C2":
                    raise ValueError("unexpected default contextual casing rule")
            elif conditions[0] not in ("lt", "tr", "az"):
                raise ValueError(f"unknown conditional casing rule: {conditions}")
            continue
        code = int(fields[0], 16)
        lower[code] = tuple(int(item, 16) for item in fields[1].split())
        upper[code] = tuple(int(item, 16) for item in fields[3].split())

    properties = {"Cased": [], "Case_Ignorable": []}
    for row in (source / "DerivedCoreProperties.txt").read_text().splitlines():
        row = row.split("#")[0].strip()
        if not row:
            continue
        fields = [item.strip() for item in row.split(";")]
        if fields[1] not in properties:
            continue
        bounds = fields[0].split("..")
        properties[fields[1]].append((int(bounds[0], 16), int(bounds[-1], 16)))

    lines = [
        "// Unicode 16.0.0 default full mappings, pinned by tools/source-font-case-data.py.",
        "// Unicode license: docs/licenses/UnicodeCase.txt. Final_Sigma is handled by FontNativeCase.hpp.",
        "struct FontCaseMapping { uint32_t Character; std::array<uint32_t, 3> Output; uint8_t Count; };",
        "struct FontCaseRange { uint32_t First, Last; };",
    ]
    for name, mappings in (("Lower", lower), ("Upper", upper)):
        entries = [(key, value) for key, value in sorted(mappings.items()) if value != (key,)]
        if any(not 1 <= len(value) <= 3 for _, value in entries):
            raise ValueError("case mapping exceeds the bounded three-scalar representation")
        lines.append(f"inline constexpr std::array<FontCaseMapping, {len(entries)}> FontCase{name} = {{{{")
        for key, value in entries:
            encoded = ", ".join(f"0x{point:x}" for point in value)
            lines.append(f"{{0x{key:x}, {{{encoded}}}, {len(value)}}},")
        lines.append("}};")

    for name, ranges in properties.items():
        merged = []
        for first, last in sorted(ranges):
            if merged and first <= merged[-1][1] + 1:
                merged[-1] = (merged[-1][0], max(last, merged[-1][1]))
            else:
                merged.append((first, last))
        symbol = name.replace("_", "")
        lines.append(f"inline constexpr std::array<FontCaseRange, {len(merged)}> FontCase{symbol} = {{{{")
        lines.extend(f"{{0x{first:x}, 0x{last:x}}}," for first, last in merged)
        lines.append("}};")
    output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "src/FontCaseTables.inc")
    options = parser.parse_args()
    generate(options.data_dir, options.output)
