"""Prototype byte signatures for the Tiberium/overlay-icon hooks on all four builds."""
import re
import sys

import pefile

ROOT_TW = r"D:\SteamLibrary\steamapps\common\Command and Conquer 3 Tiberium Wars\RetailExe"
ROOT_KW = r"D:\SteamLibrary\steamapps\common\Command and Conquer 3 - Kane's Wrath\RetailExe"
BUILDS = {
    "TW 1.10": ROOT_TW + r"\1.10\cnc3game.dat",
    "TW 1.9": ROOT_TW + r"\1.9\cnc3game.dat",
    "KW 1.03": ROOT_KW + r"\1.3\cnc3ep1.dat",
    "KW 1.02": ROOT_KW + r"\1.2\cnc3ep1.dat",
}

_images = {}


def image(name):
    if name not in _images:
        pe = pefile.PE(BUILDS[name], fast_load=True)
        _images[name] = (pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase)
    return _images[name]


def compile_pattern(text):
    parts = []
    for tok in text.split():
        parts.append(b"." if tok == "??" else re.escape(bytes([int(tok, 16)])))
    return re.compile(b"".join(parts), re.S)


def find(name, text):
    data, _ = image(name)
    return [m.start() for m in compile_pattern(text).finditer(data)]


def rel_target(data, at):
    return at + 5 + int.from_bytes(data[at + 1:at + 5], "little", signed=True)


def calls_to(name, target):
    data, _ = image(name)
    out = []
    for m in re.finditer(b"\xE8", data):
        at = m.start()
        if at + 5 <= len(data) and rel_target(data, at) == target:
            out.append(at)
    return out


def hexdump(name, rva, n):
    data, _ = image(name)
    return " ".join("%02X" % b for b in data[rva:rva + n])


if __name__ == "__main__":
    exec(open(sys.argv[1]).read())
