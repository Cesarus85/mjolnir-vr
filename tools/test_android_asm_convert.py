"""Tests of tools/android_asm_convert.py, the Mach-O to ELF assembly
conversion of the Android build's guest code."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from android_asm_convert import Converter  # noqa: E402


def convert(*lines):
    return Converter(list(lines)).run().split("\n")


def test_symbols_lose_their_underscore():
    out = convert("_main:", "\tbl\t_foo", "\t.long\t_bar")
    assert out[:3] == ["main:", "\tbl foo", "\t.long bar"]


def test_strings_are_kept():
    # (a string starting with "_", or one with \" in it, lost a "_": Opus's
    # pulse cache was one byte short, and its encoder crashed)
    out = convert(
        '\t.asciz\t"_lan_"',
        '\t.ascii\t"\\"$&\'\\0063_\\206"',
        '\t.string\t"l_.str _foo"',
    )
    assert out[:3] == [
        '\t.asciz "_lan_"',
        '\t.ascii "\\"$&\'\\0063_\\206"',
        '\t.string "l_.str _foo"',
    ]
