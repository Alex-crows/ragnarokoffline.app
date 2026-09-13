#!/usr/bin/env python3
"""Wire companion commands into party chat without depending on line offsets."""

from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text(encoding="utf-8")
hook = "\tpopulation_engine_on_party_chat(sd, message);"
if hook not in source:
    needle = "\tparty_send_message(sd, output, strlen(output) + 1 );\n}"
    if needle not in source:
        raise SystemExit(f"party chat hook point no longer matches: {path}")
    source = source.replace(
        needle,
        "\tparty_send_message(sd, output, strlen(output) + 1 );\n\n"
        + hook
        + "\n}",
        1,
    )
    path.write_text(source, encoding="utf-8")
