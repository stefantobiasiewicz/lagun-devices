#!/usr/bin/env python3
"""Generates `components/lagunbus/include/lagunbus/registry.h` from the spec registry.

The registry in `lagunbus-spec/registry/*.yaml` is the declared source of truth. This
is the **third** implementation of the same numbers — after the hub and its node double
— and the first where copying them out by hand would be genuinely expensive: a wrong
constant on the node side only shows up through a bus analyser.

    python tools/generate_registry.py [--check]

`--check` writes nothing; it only says whether the file is up to date. Wired into the
build, it breaks it on a divergence — exactly as on the hub side.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

DEVICES = Path(__file__).resolve().parent.parent
REGISTRY_DIR = DEVICES.parent / "lagunbus-spec" / "registry"
TARGET = DEVICES / "components" / "lagunbus" / "include" / "lagunbus" / "registry.h"


FRAME_KIND_NAMES = {
    "ALARM": "ALARM",
    "ZDARZENIE": "EVENT",
    "KOMENDA": "COMMAND",
    "KOMENDA_GRUPOWA": "GROUP_COMMAND",
    "ZAPIS": "WRITE",
    "ODCZYT": "READ",
    "ODPOWIEDŹ": "RESPONSE",
    "RAPORT": "REPORT",
    "ODPOWIEDŹ_DŁUGA": "LONG_RESPONSE",
    "ZAPIS_DŁUGI": "LONG_WRITE",
    "ZAPYTANIE": "QUERY",
    "ODPOWIEDŹ_HUBA": "HUB_RESPONSE",
    "PRZYŁĄCZANIE": "JOIN",
}
"""The registry still names frame kinds in Polish, and **a Polish C identifier does not
exist**: `Ź` and `Ł` are not characters a name may be built from. The same table sits on
the hub side (`hub/tools/generate_registry.py`) and the two must agree — otherwise
`RESPONSE` on one side and `ODPOWIEDZ` on the other would be two different things in the
head of anyone reading both at once."""


def load(name: str) -> dict[str, Any]:
    """Reads YAML without a dependency: the registry is simple and regular.

    We deliberately do **not** pull in `ruamel` or `PyYAML` here. This generator is the
    only Python tool in the node repository and has to run on any machine where the
    firmware can be built — without setting an environment up first.
    """
    import re

    text = (REGISTRY_DIR / name).read_text(encoding="utf-8")
    entries: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for raw in text.split("\n"):
        line = raw.split("#")[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip():
            continue
        if re.match(r"^\s*-\s", line):
            current = {}
            entries.append(current)
            line = re.sub(r"^\s*-\s", "  ", line)
        match = re.match(r"^\s+(\w+):\s*(.*)$", line)
        if match and current is not None:
            key, value = match.group(1), match.group(2).strip()
            if value and not value.startswith(">"):
                current[key] = value.strip('"').strip("'")
    return {"entries": entries}


def numbered(name: str) -> list[tuple[str, int]]:
    """Entries that carry an `id` and a `name`. Reserved ranges are skipped — they are
    not constants."""
    out: list[tuple[str, int]] = []
    for entry in load(name)["entries"]:
        if "id" not in entry or "name" not in entry:
            continue
        label = str(entry["name"])
        if " " in label:  # "reserved, high" — a description, not an identifier
            continue
        if name == "frame-kinds.yaml":
            english = FRAME_KIND_NAMES.get(label)
            if english is None:
                raise SystemExit(
                    f"frame kind {label!r} has no translation in FRAME_KIND_NAMES — "
                    "add one in tools/generate_registry.py"
                )
            label = english
        if not label.replace("_", "").isascii():
            raise SystemExit(
                f"{label!r} is not a valid C identifier — "
                "add a translation in tools/generate_registry.py"
            )
        out.append((label, int(str(entry["id"]), 0)))
    return out


def block(name: str, doc: str, entries: list[tuple[str, int]], prefix: str, width: int) -> str:
    lines = [f"/* {doc} */"]
    for entry_name, number in entries:
        constant = f"{prefix}{entry_name.upper()}"
        lines.append(f"#define {constant:<34} 0x{number:0{width}X}u")
    return "\n".join(lines)


def names(function: str, doc: str, entries: list[tuple[str, int]], ctype: str) -> str:
    """A name table as `static inline` — the header stays self-contained.

    Without it, the traffic watch would print `0x7` instead of `RESPONSE`, and while
    chasing a fault on the bus that is the difference between "I can see what is
    happening" and "I am counting on my fingers".
    """
    lines = [
        f"/* {doc} */",
        f"static inline const char *{function}({ctype} num)",
        "{",
        "    switch (num) {",
    ]
    for label, number in entries:
        lines.append(f'    case 0x{number:02X}: return "{label}";')
    lines += ['    default: return "?";', "    }", "}"]
    return "\n".join(lines)


def build() -> str:
    parts = [
        "/* GENERATED FILE — do not edit it by hand.",
        " *",
        " * Source:    the .yaml files in lagunbus-spec/registry/",
        " * Generator: tools/generate_registry.py",
        " *",
        " * A change in the spec registry breaks the build until this file is generated",
        " * anew. That is what it is here for.",
        " */",
        "#ifndef LAGUNBUS_REGISTRY_H",
        "#define LAGUNBUS_REGISTRY_H",
        "",
        "#include <stdint.h>",
        "",
        block(
            "kinds",
            "Message kinds — the KIND field. THE NUMBER IS THE PRIORITY "
            "(05-mapowanie-can.md §2).",
            numbered("frame-kinds.yaml"),
            "LB_KIND_",
            1,
        ),
        "",
        block(
            "codes",
            "Result codes of a reply, the RESULT field, 5 bits (04-interakcje.md §8).",
            numbered("result-codes.yaml"),
            "LB_RESULT_",
            2,
        ),
        "",
        block(
            "classes",
            "Node classes — the NodeClass field in an ANNOUNCEMENT. Chooses the address "
            "block (03-adresacja.md §2.3).",
            numbered("node-classes.yaml"),
            "LB_CLASS_",
            2,
        ),
        "",
        block(
            "types",
            "Endpoint types (registry/endpoint-types.yaml).",
            numbered("endpoint-types.yaml"),
            "LB_TYPE_",
            4,
        ),
        "",
        block(
            "common",
            "Common attributes, range 0xF0-0xFF (01-model-danych.md §4).",
            numbered("common-attributes.yaml"),
            "LB_COMMON_",
            2,
        ),
        "",
        names(
            "lb_kind_name",
            "Name of a message kind — for watching traffic, not for logic.",
            numbered("frame-kinds.yaml"),
            "uint8_t",
        ),
        "",
        names(
            "lb_result_name",
            "Name of a result code. `?` means a code from outside the registry — and "
            "that has to be shown too.",
            numbered("result-codes.yaml"),
            "uint8_t",
        ),
        "",
        "#endif /* LAGUNBUS_REGISTRY_H */",
        "",
    ]
    return "\n".join(parts)


def main() -> int:
    content = build()
    if "--check" in sys.argv:
        if not TARGET.exists():
            print(f"{TARGET} does not exist — run: python tools/generate_registry.py")
            return 1
        if TARGET.read_text(encoding="utf-8") != content:
            print(
                f"{TARGET.name} has drifted from lagunbus-spec/registry/\n"
                "Run: python tools/generate_registry.py"
            )
            return 1
        print("registry is in sync")
        return 0
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(content, encoding="utf-8")
    print(f"wrote {TARGET.relative_to(DEVICES)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
