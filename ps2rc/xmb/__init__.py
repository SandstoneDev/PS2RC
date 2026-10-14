"""Your own PSP menu look for a converted game: title, icon, animated icon, background, information picture and
menu music - chosen by the player, turned into the exact files the EBOOT.PBP holds (ps2rc.pbp puts them in).

    title.py    PARAM.SFO title checks (length, characters)
    picture.py  ICON0 / PIC0 / PIC1 from any picture with a crop box (the part of the picture the player keeps)
    media.py    ffmpeg / ffprobe lookup and clip probing (user-installed or shipped as separate programs)
    clip.py     ICON1 from a GIF / MP4 / WebM clip: start, length, crop box -> 144x80 frames -> PSMF (ps2rc.pbp.icon1)
    sound.py    SND0 from WAV / MP3 / OGG: cut, fades, seamless loop -> ATRAC3plus with the external open-source
                encoder atracdenc (LGPL, a separate program) -> an AT3 file in the layout the PSP's ATRAC library reads
    project.py  the player's choices (JSON) -> the files, the shared ICON1 + SND0 size budget
    preview.py  an approximate menu mock (480x272) for the converter window: PIC1, PIC0 fade, ICON0 / ICON1, title

Pillow is needed for pictures other than exact PNGs and for the preview; ffmpeg for clips and non-WAV music;
atracdenc for SND0. Each piece says plainly what is missing.
"""
from .title import check_title, clean_title, fit_title
from .project import Project, Built, SLOT_KEYS

__all__ = ['check_title', 'clean_title', 'fit_title', 'Project', 'Built', 'SLOT_KEYS']
