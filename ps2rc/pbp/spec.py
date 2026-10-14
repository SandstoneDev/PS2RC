"""The rules this package enforces for an EBOOT.PBP and the menu assets inside it.

Every number here is a limit the packer checks. Where a behaviour of the PSP menu is not known, the text says
Unknown instead of guessing.
"""

# The eight files of a PBP, in header order (the same order as pspsdk's pack-pbp arguments).
SLOTS = ('PARAM.SFO', 'ICON0.PNG', 'ICON1.PMF', 'PIC0.PNG', 'PIC1.PNG', 'SND0.AT3', 'DATA.PSP', 'DATA.PSAR')

# Short names used by the API and the command line.
SLOT_OF = {'sfo': 'PARAM.SFO', 'icon0': 'ICON0.PNG', 'icon1': 'ICON1.PMF', 'pic0': 'PIC0.PNG',
           'pic1': 'PIC1.PNG', 'snd0': 'SND0.AT3', 'data_psp': 'DATA.PSP', 'data_psar': 'DATA.PSAR'}


class PbpError(Exception):
    """A problem the user has to fix. The message says what is wrong and how to fix it."""


class Issue:
    """One finding of a validator.

    level: 'error' - the file cannot be used (damaged, wrong container, does not fit its field);
           'spec'  - usable bytes, but outside the documented format or size (refused unless the caller
                     allows off-spec files: what the PSP menu then does is Unknown);
           'warn'  - accepted, worth a look;
           'info'  - a plain fact for the report.
    """
    LEVELS = ('error', 'spec', 'warn', 'info')

    def __init__(self, level, what, message):
        assert level in self.LEVELS, level
        self.level, self.what, self.message = level, what, message

    def __repr__(self):
        return 'Issue(%r, %r, %r)' % (self.level, self.what, self.message)

    def __str__(self):
        return '%s: %s: %s' % (self.level, self.what, self.message)


def blocking(issues, allow_off_spec=False):
    """the issues that stop a pack: errors always, 'spec' unless allowed"""
    stop = ('error',) if allow_off_spec else ('error', 'spec')
    return [i for i in issues if i.level in stop]


class ImageSpec:
    def __init__(self, slot, width, height, mode, role, required):
        self.slot, self.width, self.height, self.mode, self.role, self.required = (
            slot, width, height, mode, role, required)

    @property
    def bits(self):
        return 24 if self.mode == 'RGB' else 32


# Still images: PNG, 8 bits per channel.
ICON0 = ImageSpec('ICON0.PNG', 144, 80, 'RGB', 'still icon (also shown when ICON1 is missing or cannot play)', True)
PIC0 = ImageSpec('PIC0.PNG', 310, 180, 'RGBA', 'information picture drawn over the background, may be transparent',
                 False)
PIC1 = ImageSpec('PIC1.PNG', 480, 272, 'RGB', 'background picture', False)
IMAGES = {'icon0': ICON0, 'pic0': PIC0, 'pic1': PIC1}

# Animated icon: PSMF movie, H.264 Main profile level 2.1, 29.97 frames per second, progressive.
ICON1_W, ICON1_H = 144, 80
ICON1_FPS_NUM, ICON1_FPS_DEN = 30000, 1001
ICON1_DEFAULT_SECONDS = 10.0     # this packer's default cap for a converted clip (not a platform rule)

# Menu music: RIFF/WAVE container holding ATRAC3plus, 44.1 kHz, 2 channels.
SND0_RATE = 44100
SND0_CHANNELS = 2

# ICON1.PMF + SND0.AT3 together: 500 kilobytes. Whether 1000 or 1024 bytes per kilobyte is meant is Unknown, so
# the packer aims below the smaller value and refuses above the larger one.
MEDIA_BUDGET = 500 * 1000
MEDIA_BUDGET_MAX = 500 * 1024

# PARAM.SFO fields this package knows (type, bytes reserved by Sony-style files where known, meaning).
SFO_KEYS = {
    'TITLE': ('str', 128, 'name shown in the menu, UTF-8, one line'),
    'DISC_ID': ('str', 16, 'product id, A-Z and 0-9 only'),
    'DISC_VERSION': ('str', 8, 'version, "X.XX"'),
    'CATEGORY': ('str', 4, 'content category; homebrew built with pspsdk uses "MG"'),
    'PARENTAL_LEVEL': ('int', 4, 'parental control level 0..11'),
    'PSP_SYSTEM_VER': ('str', 8, 'system software version needed, "X.XX"'),
    'REGION': ('int', 4, 'region bits; pspsdk writes 0x8000'),
    'BOOTABLE': ('int', 4, '1 = can be started'),
    'MEMSIZE': ('int', 4, '1 = ask for the large user memory partition (PSP-2000 and later)'),
    'APP_VER': ('str', 8, 'application version, "XX.XX"'),
    'USE_USB': ('int', 4, '1 = uses USB peripherals'),
}
TITLE_MAX = 127          # bytes of UTF-8 text (the field holds 128 with the terminating zero)
DISC_ID_MAX = 12         # characters (A-Z, 0-9)
PARENTAL_MAX = 11
