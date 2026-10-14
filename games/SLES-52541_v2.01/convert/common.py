"""Shared helpers of this pack's conversion steps: the boot program and its code overlays as images, the function
discovery files of the conversion's work folder.

The converter runs the steps with the conversion's local.toml (work folder, disc folder); nothing here is written
outside the work folder."""
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK_DIR = os.path.dirname(HERE)
ROOT = os.path.abspath(os.path.join(PACK_DIR, "..", ".."))
sys.path.insert(0, ROOT)
from ps2rc import config, pack as _pack  # noqa: E402
from ps2rc.image import Image, Segment, load_elf  # noqa: E402
from ps2rc.overlay import load_overlay  # noqa: E402
from ps2rc.analysis.functions import FunctionSet, discover_functions  # noqa: E402
from ps2rc.analysis.vucode import find_vu_regions  # noqa: E402
from ps2rc.r5900 import decode  # noqa: E402

PACK_NAME = os.path.basename(PACK_DIR)
PACK = _pack.load(PACK_NAME)
WORK = config.work_dir()                         # the conversion's work folder (game-derived files)
DISC_DIR = config.disc_dir(PACK_NAME, required=False) or ""
BOOT_ELF = os.path.join(DISC_DIR, PACK.get("id.boot_elf")) if DISC_DIR else ""
OVERLAY_NAMES = list(PACK.get("overlays.files"))
CODE_LO = PACK.get("memory.code")[0]
CTOR_CODE = tuple(PACK.get("memory.ctor_code"))
OVERLAY_WINDOW = tuple(PACK.get("overlays.window"))
FUNCTIONS = PACK.path("translate.functions")


def say(*a):
    print(*a, flush=True)


def main_image():
    """The stripped boot program and its code ranges: main code up to the VU microcode, plus the static-constructor
    code after .data."""
    img = load_elf(BOOT_ELF).stripped()
    vu = min(lo for lo, _ in find_vu_regions(img))
    return img, [(CODE_LO, vu), CTOR_CODE]


def overlay_image(img, name):
    """(overlay, image with the overlay mapped at its window, its .text range)."""
    ov = load_overlay(os.path.join(DISC_DIR, name))
    mem = ov.memory()
    seg = Segment(vaddr=ov.load_addr, filesz=len(ov.raw), memsz=len(mem), flags=7, data=mem)
    oimg = Image(path=ov.path, entry=ov.text_addr, gp=img.gp, segments=list(img.segments) + [seg], overlay_slots=[])
    return ov, oimg, [(ov.text_addr, ov.text_addr + ov.text_size)]


def discover_overlays(img):
    """{overlay name: (image, code ranges, FunctionSet)}."""
    out = {}
    for name in OVERLAY_NAMES:
        _ov, oimg, code = overlay_image(img, name)
        out[name] = (oimg, code, discover_functions(oimg, code))
    return out


def load_functions(path=FUNCTIONS):
    with open(path, "rb") as f:
        return pickle.load(f)


__all__ = ["FunctionSet", "Image", "Segment", "decode", "discover_functions", "load_elf", "load_overlay"]
