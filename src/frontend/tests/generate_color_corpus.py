#!/usr/bin/env python3
"""Regenerate the Rust color-parity corpus and the CSS3 keyword table.

The oracle is the installed tinycss2:

    from tinycss2.color3 import parse_color

plus the archived Python oracle's `argb()` math (banker's rounding
of `channel * 255`, alpha 0 -> 0).  Run it from the repository root with the
project venv:

    .venv\\Scripts\\python.exe src/frontend/tests/generate_color_corpus.py

It rewrites `src/frontend/tests/color_corpus.rs` and prints the
`NAMED_COLORS` table for `src/frontend/src/style/color.rs` on stdout.

`currentcolor` is deliberately absent from the corpus: tinycss2 returns the
sentinel string `currentColor`, and Python's `argb()` then raises
AttributeError.  Rust documents that bug and returns 0 instead (unit test).
"""

from __future__ import annotations

import sys
from pathlib import Path

import tinycss2
import tinycss2.color3 as color3

HERE = Path(__file__).resolve().parent
OUT = HERE / "corpus" / "color_corpus.rs"


def argb(value: str) -> int:
    """Oracle `argb()` for a string; invalid/empty -> 0, masked to 32 bits."""
    if not value:
        return 0
    color = color3.parse_color(value)
    if color is None or not isinstance(color, color3.RGBA) or color.alpha <= 0:
        return 0
    a = round(color.alpha * 255)
    r = round(color.red * 255)
    g = round(color.green * 255)
    b = round(color.blue * 255)
    # Python ints are unbounded; the C contract is a 32-bit ARGB.  Only
    # out-of-range channels (rgb(300,0,0), rgb(-10,0,0)) can leave 0..2**32.
    return (a << 24) | (r << 16) | (g << 8) | b


def oracle_parse(value: str) -> str | None:
    """`cascade.parse_color()`: opaque `#rrggbb`, alpha 0 -> None.

    Out-of-range channels are skipped: Python's `"%02x" % 300` produces an
    invalid string, while Rust clamps the normalized form (documented in
    color.rs).  Their ARGB still matches and is covered by `CASES`.
    """
    if not value:
        return None
    color = color3.parse_color(value)
    if color is None or not isinstance(color, color3.RGBA) or color.alpha <= 0:
        return None
    channels = [round(color.red * 255), round(color.green * 255), round(color.blue * 255)]
    if any(channel < 0 or channel > 255 for channel in channels):
        return None
    return "#%02x%02x%02x" % tuple(channels)


def rust_string(value: str) -> str:
    out = ['"']
    for ch in value:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ord(ch) < 32 or ord(ch) == 127:
            out.append("\\u{%x}" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def cases() -> list[str]:
    values: list[str] = []

    # Every CSS3 keyword (147 names) plus `transparent`.  Sorted for a stable
    # diff; `currentcolor` is excluded on purpose (see module docstring).
    for keyword in sorted(color3._COLOR_KEYWORDS):
        if keyword != "currentcolor":
            values.append(keyword)

    values += [
        # case and whitespace around an ident
        "RED", "Red", " red ", "  red  ", "\tred\n", "\n red \t",
        # hex: 3/4/6/8 digits, both cases, invalid lengths/digits
        "#abc", "#ABC", "#abcd", "#ABCD", "#aabbcc", "#AABBCC",
        "#aabbccdd", "#AABBCCDD", "#000", "#0000", "#000000", "#00000000",
        "#fff", "#ffff", "#ffffff", "#ffffffff", "#123", "#1234",
        "#123456", "#12345678", "#f0f8ff", "#FAEBD7", "#7f8da3",
        "#00ff0080", "#ff0000ff", "#ff000000", "#ff000001",
        "#ggg", "#GGG", "#12", "#12345", "#1234567", "#123456789",
        "#", "##abc", "abcdef",
        # rgb(): commas mandatory, 3 integer channels, percent channels
        "rgb(255,0,0)", "rgb( 255 , 0 , 0 )", "RGB(255,0,0)", "RgB(0,0,0)",
        "rgb(+255,+0,+1)", "rgb(0,0,0)", "rgb(255,255,255)",
        "rgb(1,2,3)", "rgb(0%,0%,0%)", "rgb(100%,100%,100%)",
        "rgb(50%,25%,10%)", "rgb(50%,50%,50%)", "rgb(30%,30%,30%)",
        "rgb(70%,70%,70%)", "rgb(100%,120%,0%)", "rgb(-10%,0%,0%)",
        "rgb(-10,0,0)", "rgb(300,0,0)", "rgb(0,0,300)",
        "rgb(255.0,0,0)", "rgb(2.55e2,0,0)", "rgb(1e2,0,0)",
        "rgb(255,0)", "rgb(255,0,0,1)", "rgb(1,2,3,4)",
        "rgb(255 0 0)", "rgb(255 0 0 / 50%)", "rgb(255,0,0/)",
        "rgb(255,0,0,)", "rgb(,255,0,0)", "rgb()", "rgb(  )", "rgb(,,,)",
        "rgb(1,,2,3)", "rgb(none,0,0)", "rgb(#fff,0,0)",
        "rgb(1,2,3) red", "rgb(255,0,0)/*x*/", "rgb(/*x*/255,0,0)",
        "rgb(1px,0,0)", "rgb(1deg,1%,1%)",
        # rgba(): 4 args, alpha is a bare number (percent rejected)
        "rgba(0,0,0,0.5)", "rgba(255,0,0,0.25)", "rgba(0,0,0,0)",
        "rgba(0,0,0,1)", "rgba(0,0,0,2)", "rgba(0,0,0,-1)",
        "rgba(0,0,0,.5)", "rgba(0,0,0,0.3)", "rgba(0,0,0,0.7)",
        "rgba(0,0,0,0.1)", "rgba(0,0,0,0.9)", "rgba(0,0,0,1e-1)",
        "rgba(0,0,0,50%)", "rgba(1,2,3)", "rgba(1,2,3,4,5)",
        "rgba(255,0,0,0.5,)", "RGBA(255,0,0,0.5)",
        "rgba(50%,50%,50%,0.5)", "rgba(1.5,0,0,0.5)",
        "rgba(255,0,0,0.5) red",
        # hsl(): unitless hue only, s/l as percentages
        "hsl(0,100%,50%)", "hsl(120,100%,25%)", "hsl(120,100%,75%)",
        "hsl(240,100%,50%)", "hsl(360,100%,50%)", "hsl(-120,100%,50%)",
        "hsl(720,100%,50%)", "hsl(0.5,100%,50%)", "hsl(+120,100%,50%)",
        "hsl(1e2,100%,50%)", "hsl(0,0%,0%)", "hsl(0,0%,100%)",
        "hsl(180,50%,50%)", "hsl(30,100%,50%)", "hsl(210,60%,40%)",
        "hsl(300,80%,60%)", "hsl(45,100%,50%)", "hsl(270,100%,50%)",
        "hsl(90,100%,50%)", "hsl(150,100%,50%)", "hsl(330,100%,50%)",
        "hsl(120, 100%, 50%)", "HSL(120,100%,50%)",
        "hsl(0deg,100%,50%)", "hsl(120deg,100%,50%)", "hsl(0turn,100%,50%)",
        "hsl(0rad,100%,50%)", "hsl(0grad,100%,50%)",
        "hsl(0,100,50)", "hsl(0,100%,50%,0.5)", "hsl(0,100%,50%,)",
        "hsl(120 100% 50%)", "hsl(120,100%,50%) red",
        # hsla(): 4 args, percent alpha rejected
        "hsla(0,100%,50%,0.25)", "hsla(240,100%,50%,0.5)",
        "hsla(120,100%,25%,1)", "hsla(0,100%,50%,50%)",
        "hsla(0,100%,50%)", "hsla(0,100%,50%,0.5,1)",
        # alpha / channel rounding ties and clamps
        "rgba(255,0,0,0.5)", "rgba(0,0,0,0.5)", "rgba(0,0,0,0.0625)",
        # alpha > 0 but rounding to a 0 alpha byte (0x00RRGGBB)
        "rgba(255,0,0,0.001)", "rgba(255,0,0,0.002)",
        "rgba(255,0,0,0.0019)", "rgba(255,0,0,0.0039)",
        # hsl channels outside 0..1 (colorsys does not clamp them)
        "hsl(0,120%,50%)", "hsl(0,-10%,50%)", "hsl(0,100%,-10%)",
        "hsl(0,100%,110%)",
        "rgb(127,127,127)", "rgb(128,128,128)",
        # named keywords that are not CSS3 colors, plus the transparent alias
        "currentcolor", "rebeccapurple", "none", "initial", "inherit",
        "unset", "transparent", "TRANSPARENT", " transparent ",
        "grey", "gray", "GREY",
        # garbage / non-color component values
        "", " ", "  ", "\t", "\n", "2.55e2", "0", "-1", "1e2", "red red",
        "red;", "red!", "red,", "/*x*/red", "red/*x*/", "/*x*/ red /*x*/",
        "/*unterminated red", "rgb", "rgb(", "rgb(255,0,0", "hsl(0,100%,50%",
        "url(#x)", "var(--c)", "#fff #000", "red blue",
    ]

    values += fuzz_cases()
    return values


def fuzz_cases() -> list[str]:
    """Deterministic random component-value soup (documented gaps excluded).

    CSS escapes (``\\72 ed``) are not covered: they would need tinycss2's full
    identifier/unescape pipeline.
    """
    import random

    rng = random.Random(20261125)
    atoms = [
        "0", "1", "2", "5", "10", "50", "100", "255", "256", "300", "-1",
        "-10", "+1", "+255", ".5", "0.5", "1.5", "255.0", "1e2", "2.55e2",
        "1E1", "0%", "50%", "100%", "120%", "-10%", "+5%",
        "red", "RED", "none", "transparent", "currentcolor", "rebeccapurple",
        "black", "white", "grey", "aliceblue",
        "#fff", "#FFF", "#0000", "#123456", "#12345678", "#ggg", "#",
        "rgb", "rgba", "hsl", "hsla", "RGB", "Hsl", "url", "var",
        "(", ")", ",", " ", "  ", "\t", "/", "deg", "turn", "px", "%",
        "/*c*/", "/*", "*/", ";", "!", "-", "+", ".", "0deg", "1px",
    ]
    names = ["rgb", "rgba", "hsl", "hsla", "RGB", "Hsl", "url", "var"]
    cases: list[str] = []
    for _ in range(250):
        if rng.random() < 0.65:
            name = rng.choice(names)
            count = rng.choice([0, 1, 2, 3, 4, 5, 6])
            arguments = [rng.choice(atoms) for _ in range(count)]
            joiner = rng.choice([",", ", ", " ", ",,", "", " , "])
            text = name + "(" + joiner.join(arguments) + ")"
        else:
            text = rng.choice(atoms) + rng.choice(["", " ", ""]) + rng.choice(atoms)
        # Keep the corpus meaningful: at least one non-whitespace character.
        if text.strip():
            cases.append(text)
    return cases


def emit_corpus() -> None:
    rows = []
    normalized_rows = []
    seen: set[str] = set()
    for value in cases():
        if value in seen:
            continue
        seen.add(value)
        # `currentcolor` makes the oracle's argb() raise AttributeError: it has no
        # ARGB to record.  The Rust unit test covers the documented 0 instead.
        if isinstance(color3.parse_color(value), str):
            continue
        expected = argb(value) & 0xFFFF_FFFF
        rows.append(
            "    (" + rust_string(value) + ", 0x%08X)," % expected
        )
        normalized = oracle_parse(value)
        if normalized is not None:
            normalized_rows.append(
                "    (" + rust_string(value) + ", " + rust_string(normalized) + "),"
            )
    header = (
        "// @generated by src/frontend/tests/generate_color_corpus.py -- do not edit.\n"
        "//\n"
        "// Oracle: tinycss2 %s `parse_color` plus the archived `argb()`\n"
        "// (round-half-even of channel*255, alpha 0 -> 0).  Expected values are\n"
        "// masked to the C `unsigned int` 32-bit contract; only out-of-range\n"
        "// channels (rgb(300,0,0)) differ from Python's unbounded integers.\n"
        "//\n"
        "// `currentcolor` is not in the corpus: Python's `argb()` raises\n"
        "// AttributeError on the sentinel string.  Rust returns 0 (see the unit\n"
        "// test in src/style/color.rs).\n"
        "\n"
        "pub(crate) const CASES: &[(&str, u32)] = &[\n" % tinycss2.VERSION
    )
    text = header + "\n".join(rows) + "\n];\n"
    text += (
        "\n"
        "// `cascade.parse_color()` (v1 opaque normalization). Out-of-range\n"
        "// channels are absent here (Python emits invalid CSS for them);\n"
        "// their ARGB is covered above.\n"
        "pub(crate) const NORMALIZED_CASES: &[(&str, &str)] = &[\n"
        + "\n".join(normalized_rows)
        + "\n];\n"
    )
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({len(rows)} argb cases, {len(normalized_rows)} normalized cases)")


def emit_keyword_table() -> None:
    print("// @generated from tinycss2.color3._COLOR_KEYWORDS (excluding")
    print("// currentcolor/transparent, which color.rs matches explicitly).")
    print("const NAMED_COLORS: &[(&str, u32)] = &[")
    for keyword in sorted(color3._COLOR_KEYWORDS):
        if keyword in ("currentcolor", "transparent"):
            continue
        color = color3._COLOR_KEYWORDS[keyword]
        r = round(color.red * 255)
        g = round(color.green * 255)
        b = round(color.blue * 255)
        print('    ("%s", 0x%02X%02X%02X),' % (keyword, r, g, b))
    print("];")


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--table":
        emit_keyword_table()
    else:
        emit_corpus()
    return 0


if __name__ == "__main__":
    sys.exit(main())
