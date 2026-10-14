"""EBOOT.PBP / PARAM.SFO tools with user-supplied menu assets (ICON0, ICON1, PIC0, PIC1, SND0).

Pure Python. Optional: Pillow (to convert pictures), ffmpeg with libx264 (with ps2rc.movies, to make ICON1.PMF
from a video). See README.md in this folder for the API and the command line (`python -m ps2rc.pbp`).
"""
from .spec import PbpError, Issue, SLOTS, ICON0, PIC0, PIC1, MEDIA_BUDGET, MEDIA_BUDGET_MAX, blocking
from .pbp import Pbp, module_kind
from .sfo import Sfo, make as make_sfo
from .assets import apply, set_params, check, describe
from . import at3, icon1, image, png, placeholder

__all__ = ['PbpError', 'Issue', 'SLOTS', 'ICON0', 'PIC0', 'PIC1', 'MEDIA_BUDGET', 'MEDIA_BUDGET_MAX', 'blocking',
           'Pbp', 'module_kind', 'Sfo', 'make_sfo', 'apply', 'set_params', 'check', 'describe',
           'at3', 'icon1', 'image', 'png', 'placeholder']
