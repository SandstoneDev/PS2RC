"""Texts of the converter window. English is here (the default); another language comes from
gui/lang/<code>.json: a JSON object with the same keys, plus "_name" (the language's own name), "_errors" (the
ERRORS table) and "_steps" (step titles). A missing text falls back to English; `%s` placeholders must match."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

T = {
    "app": "PS2RC Converter",
    "back": "< Back",
    "next": "Next >",
    "home": "Start page",
    "skip": "Skip >",
    "lang": "Language",
    "lang.system": "System default",
    "menu.view": "View",
    "menu.size": "Interface size",
    "size.small": "Small",
    "size.normal": "Normal",
    "size.large": "Large",
    "steps": ("Disc", "Output", "Customize", "Settings", "Convert"),
    "home.h": "What do you want to do?",
    "home.sub": "Everything happens on this PC. Nothing is uploaded; the result is for your own PSP.",
    "home.convert": "Convert my disc",
    "home.convert.sub": "Your own PS2 disc image -> a game folder for the PSP",
    "home.check": "Check / update a PSP folder",
    "home.check.sub": "Check the files of a converted game, repair or update it",
    "home.version": "Converter version %s",
    "disc.h": "Which disc?",
    "disc.sub": ("Choose the image of YOUR OWN PS2 game disc (.iso). Make it from your disc with any disc imaging "
                 "program."),
    "disc.pick": "Choose disc image...",
    "disc.folder": "or a disc folder...",
    "disc.none": "No disc chosen yet.",
    "disc.checking": "Checking the disc...",
    "disc.ok": "Supported: %s (%s, version %s)",
    "disc.folder_ok": "Disc folder: it is checked when the conversion starts.",
    "disc.bad": "This disc cannot be converted: %s",
    "disc.types": "Disc images",
    "out.h": "Where should the game go?",
    "out.sub": ("Connect your PSP with a USB cable and choose \"USB Connection\" on it - it shows up here. Or write "
                "to a folder on this PC and copy it later."),
    "out.psp": "PSP / memory card %s  (%s free)",
    "out.drive": "Removable drive %s  (%s free, no PSP folder yet)",
    "out.pc": "A folder on this PC:",
    "out.browse": "Browse...",
    "out.refresh": "Look for a PSP again",
    "out.none": "No PSP found. Connect it (USB Connection on the PSP) and press \"Look for a PSP again\".",
    "out.target": "The game will be written to:  %s",
    "out.exists": "A converted game is already there; it will be brought up to date.",
    "out.slow": "Writing straight to the PSP is slower than to the PC (USB 2.0).",
    "out.need": "Choose where the game should go.",
    "cus.h": "Your own look in the PSP menu (optional)",
    "cus.tab.title": "Title and icon",
    "cus.tab.icon1": "Animated icon",
    "cus.tab.pic1": "Background",
    "cus.tab.pic0": "Picture",
    "cus.tab.snd0": "Music",
    "cus.title": "Title in the PSP menu",
    "cus.title.default": "(empty = %s)",
    "cus.title.bytes": "%d of %d bytes",
    "title.empty": "",
    "title.control": "Line breaks and control characters are not allowed.",
    "title.too_long": "Too long (%s bytes): shorten it.",
    "title.spaces": "Extra spaces are removed.",
    "title.chars": "These characters may not show on the PSP: %s",
    "title.long": "Long title (%s characters): the menu may cut it.",
    "slot.none": "None",
    "slot.default": "Default",
    "slot.file": "My file:",
    "slot.browse": "Choose...",
    "slot.crop": "Crop...",
    "slot.busy": "making...",
    "slot.ok": "ready, %s",
    "icon0.h": "Icon",
    "adv.h": "More options",
    "adv.movies": "Movies",
    "adv.audio": "Sound files",
    "adv.jobs": "Compile jobs",
    "icon0.hint": "Still icon, 144x80. Any picture: you choose the part with the crop box.",
    "icon1.hint": "Plays while the game is selected, 144x80, loops. From a GIF, MP4, WebM or other clip.",
    "pic1.hint": "Full-screen background, 480x272.",
    "pic0.hint": "Picture over the background, 310x180, may be transparent (PNG).",
    "snd0.hint": "Music while the game is selected; plays in a loop. From WAV, MP3, OGG, FLAC.",
    "range.start": "Start, s",
    "range.len": "Length, s",
    "snd0.loop": "Seamless loop",
    "snd0.listen": "Listen",
    "snd0.stop": "Stop",
    "snd0.off": "Music is OFF: %s",
    "snd0.offhint": ("Menu music needs the free ATRAC3plus encoder \"atracdenc\". The converter app brings it in "
                     "tools/atracdenc when it is included; a ready .at3 file also works."),
    "snd0.offmf": ("This Windows has no Media Foundation (a Windows N / KN edition); the music encoder needs it. "
                   "Install Microsoft's free Media Feature Pack: %s"),
    "path.unsafe": ("The PSP compiler cannot use these folders on this Windows:\n%s\n\nThe converter folder "
                    "must have only English letters, digits and spaces in its path (for example a folder PS2RC "
                    "directly on drive C). The output folder may also use the letters of your Windows language."),
    "conv.jobs": "Compiling with %d jobs at a time to save memory.",
    "out.foreign_note": "A folder of that name is already there and was not made by this converter: it will not be "
                        "overwritten (you will be offered another name).",
    "out.foreign": ("%s already exists and was not made by this converter, so it will not be overwritten.\n\n"
                    "Use the folder name %s instead?"),
    "menu.help": "Help",
    "about.h": "About PS2RC Converter",
    "about.what": "Converts your own PS2 game disc into a PSP game folder, on this PC.",
    "about.licence": "Licence: PS2RC License 1.0 (licenses/PS2RC-LICENSE.txt).",
    "about.credits": "Made by SandstoneDev.",
    "about.project": "Project:",
    "about.discord": "Join the community on Discord:",
    "about.third": ("Third-party programs in this folder (Python, Qt for Python, the PSP toolchain, ffmpeg, "
                    "atracdenc) keep their own licences: their texts, and where their source code is, are in the "
                    "licenses folder (the toolchain's in toolchain/licenses). On request we also send the source "
                    "code of the shipped open-source programs (see NOTICE.md there)."),
    "about.licences": "Third-party licences...",
    "about.close": "Close",
    "adv.movies.auto": "auto",
    "adv.movies.skip": "skip",
    "adv.movies.convert": "convert",
    "adv.audio.all": "all",
    "adv.audio.min": "minimum",
    "adv.audio.none": "none",
    "why.missing": "missing",
    "why.damaged": "damaged",
    "why.ok": "OK",
    "why.new": "new in this converter version",
    "why.changed": "changed in this converter version",
    "why.yours": "your settings",
    "why.merged": "your settings kept, new settings added",
    "why.template": "missing: written from the template",
    "why.unused": "made by an older version, not used any more",
    "why.runtime": "the runtime or the game pack changed since it was made",
    "need.ffmpeg": "needs ffmpeg (tools/ffmpeg in the converter app)",
    "budget": "Animated icon %s + music %s = %s of 500 KB",
    "budget.over": "Too big together: shorten the music or the clip.",
    "budget.hint": "The animated icon and the music share 500 KB; the icon's quality adapts to what is left.",
    "pv.title": "Preview (approximate PSP menu)",
    "pv.replay": "Replay",
    "pv.sound": "Music in preview",
    "pv.nopillow": "The preview needs Pillow (the converter app ships it).",
    "cus.reset": "Reset look to default",
    "crop.h": "Drag the frame; drag a corner or use the mouse wheel to resize.",
    "crop.ok": "OK",
    "crop.cancel": "Cancel",
    "crop.reset": "Centre",
    "pick.pictures": "Pictures",
    "pick.clips": "Clips and GIFs",
    "pick.music": "Music",
    "pick.all": "All files",
    "set.h": "Game settings",
    "set.sub": "Our values run best on a PSP. You can change them later in ps2rc.cfg in the game folder.",
    "set.none": "This game has no settings file template here: the game's defaults are used.",
    "set.nodisc": "Choose a supported disc first: the settings come with the game.",
    "set.restore": "Restore our defaults",
    "set.changed": "changed",
    "set.notset": "(not set)",
    "set.ours": "ours: %s",
    "set.risk.h": "Are you sure?",
    "set.risk": "%s\n\nChange it anyway?",
    "set.ourvalues": "our values",
    "set.count": "%d setting(s) changed",
    "conv.h": "Ready to convert",
    "conv.disc": "Disc",
    "conv.out": "Output",
    "conv.title": "Title",
    "conv.look": "Menu look",
    "conv.settings": "Settings",
    "conv.look.none": "standard",
    "conv.go": "Convert",
    "conv.stop": "Stop",
    "conv.left": "Time left: %s",
    "conv.step": "Step %d of %d: %s",
    "conv.kept": "kept from the last run",
    "conv.details": "Show details",
    "conv.done": "Done! The game is in %s",
    "conv.done.psp": ("On the PSP: XMB -> Game -> Memory Stick. Disconnect the USB cable first (\"Safely remove\" on "
                      "Windows)."),
    "conv.done.pc": "Copy this folder to PSP/GAME/ on the memory stick.",
    "conv.open": "Open folder",
    "conv.again": "Try again",
    "conv.running": "A conversion is running. Stop it and close?\n(Finished steps are kept; the next run continues.)",
    "conv.stopping": "Stopping after the current command...",
    "conv.making_look": "Preparing the menu look...",
    "conv.look_err": "Part of the menu look could not be made and is left out: %s",
    "err.what": "What to do: %s",
    "err.title": "The conversion stopped",
    "chk.h": "Check or update a converted game",
    "chk.found": "Converted games found:",
    "chk.none": "No converted game found on a connected PSP. Connect it, or choose a folder.",
    "chk.col.title": "Game",
    "chk.col.where": "Folder",
    "chk.col.ver": "Made by version",
    "chk.col.state": "Status",
    "chk.update_av": "update available",
    "chk.uptodate": "up to date",
    "chk.unknown": "unknown",
    "chk.pick": "Choose a folder...",
    "chk.check": "Check files",
    "chk.update": "Update...",
    "chk.select": "Select a game in the list first.",
    "chk.running": "Checking %s ...",
    "chk.allok": "All %s files are OK.",
    "chk.allok2": "All files are OK.",
    "chk.bad": "%d file(s) damaged or missing.",
    "chk.repair": "Repair...",
    "chk.user": "Changed by you (kept): %d",
    "chk.na": "Not possible yet: %s",
    "upd.h": "Update: what will change",
    "upd.planning": "Comparing the folder with this converter...",
    "upd.col.file": "File",
    "upd.col.do": "Action",
    "upd.col.why": "Why",
    "upd.nothing": "Nothing to do: the folder is up to date.",
    "upd.needdisc": "Some files must be made again from your disc: choose the disc image.",
    "upd.saves": "Your saves and your settings (ps2rc.cfg) are never touched.",
    "upd.go": "Update now",
    "upd.done": "Update finished.",
    "act.copy": "copy again",
    "act.rebuild": "make again",
    "act.merge_cfg": "add new settings",
    "act.keep": "keep",
    "act.write": "write",
    "act.remove": "remove",
    "act.rename": "keep as .old",
    "gal.h": "Pictures from your game disc",
    "gal.show": "Show:",
    "gal.all": "All",
    "gal.loading": "Loading screens",
    "gal.title": "Title and splash screens",
    "gal.background": "Menu backgrounds",
    "gal.logo": "Logos",
    "gal.other": "Other pictures",
    "gal.note": "Read from your own disc image now; the pictures stay on this PC.",
    "gal.use": "Use this picture",
    "gal.use_all": "Use game artwork",
    "gal.use_all_tip": "One click: a loading screen of your disc as the background and an icon cut from it",
    "gal.from_disc": "From the game disc...",
    "gal.tip": "Pick a loading screen, title screen or menu background from your disc image",
    "gal.reading": "Reading the disc...",
    "gal.none": "No pictures found on this disc image.",
    "snd0.q": "Quality / length:",
    "snd0.q.best": "Best quality (ATRAC3plus 352k, ~%d s)",
    "snd0.q.balanced": "Balanced (ATRAC3 132k, ~%d s)",
    "snd0.q.longest": "Longest (ATRAC3 66k, ~%d s)",
    "snd0.q.tip": ("ATRAC3 is outside Sony's documented menu-music format (ATRAC3plus), but it played in the menu "
                   "of a PSP-3000 with custom firmware in our test; other PSP models are untested. Animated icon "
                   "and music share the 500 KB."),
    "gal.reading_n": "Reading pictures from your game disc... %d so far",
    "gal.count": "%d pictures from your disc",
    "mus.h": "Music from your game disc",
    "mus.reading": "Reading the list of the game's music from your disc...",
    "mus.reading_n": "Reading the game's music list... %d of %d",
    "mus.count": "%d pieces of music on your disc",
    "mus.search": "Search by name",
    "mus.col.name": "Name",
    "mus.col.kind": "Kind",
    "mus.col.len": "Length",
    "mus.start": "Start at:",
    "mus.note": ("Read from your own disc image now; nothing leaves this PC. The piece is shortened to what fits "
                 "the 500 KB menu budget on the Music tab."),
    "mus.use": "Use this music",
    "mus.radio": "Radio",
    "mus.cutscene": "Cutscene",
    "mus.beats": "Beats",
    "mus.ambience": "Ambience",
    "mus.adverts": "Adverts",
    "mus.other": "Other",
    "mus.from_disc": "From the game disc...",
    "mus.taking": "Taking the music from your disc...",
    "mus.listening": "Preparing the music...",
    "gal.busy": "Reading your game disc... (the pictures appear as they are found)",
    "chk.scanning": "Looking for a PSP and converted games...",
    "log.copy": "Copy log",
    "log.open": "Open log file",
    "log.verbose": "Show every line (verbose)",
    "log.more": "... %d more lines of the translator (right-click > Show every line)",
    "eta.lt1": "less than a minute",
    "eta.min": "about %d min",
}

# plain-language errors: code -> (what happened, what to do)
ERRORS = {
    "input": ("The disc image cannot be read.",
              "Choose the .iso image of your disc again (is it complete?)."),
    "not_ps2": ("This is not a PS2 game disc image.",
                "Choose the image of a PS2 game disc."),
    "no_pack": ("This game is not supported by this converter.",
                "Check the list of supported games in the README."),
    "version": ("This version of the game is not supported.",
                "Use the disc version named in the README."),
    "disk": ("Not enough free space.",
             "Free some space or choose another place (a PC folder)."),
    "no_toolchain": ("The compiler is missing.",
                     "Use the complete converter app (it includes the compiler), or install it as the README says."),
    "toolchain": ("The compiler does not work.",
                  "Re-download the converter app; antivirus programs sometimes block it."),
    "compile": ("Building the game program failed.",
                "Press \"Try again\". If it fails again, send the log file named below."),
    "assets": ("A menu picture / clip / music could not be used.",
               "Choose another file on the Customize page, or set that item to None."),
    "stage": ("Copying the game files failed.",
              "Check the PSP / card is still connected and has space; press \"Try again\"."),
    "verify": ("Some written files are not correct.",
               "The card or the cable may be faulty: press \"Try again\" (only the bad files are written again)."),
    "cancelled": ("Stopped.",
                  "Press Convert to continue: finished steps are kept."),
    "internal": ("An unexpected error happened.",
                 "Please report it with the log (Show details)."),
}


def _load(code):
    try:
        with open(os.path.join(HERE, "lang", code + ".json"), encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


TEXTS = {"en": {}}
for _fn in sorted(os.listdir(os.path.join(HERE, "lang"))) if os.path.isdir(os.path.join(HERE, "lang")) else []:
    if _fn.endswith(".json") and _fn[:-5] != "en":
        TEXTS[_fn[:-5]] = _load(_fn[:-5])
LANGS = tuple([("en", "English")] + [(c, d.get("_name") or c) for c, d in sorted(TEXTS.items()) if c != "en"])


def tr(key, lang="en", *args):
    d = TEXTS.get(lang) or {}
    s = d.get(key, T.get(key, key))
    if isinstance(s, list):
        s = tuple(s)
    if args and isinstance(s, str):
        try:
            return s % args
        except (TypeError, ValueError):
            return s
    return s


def error_text(code, message, hint, lang="en"):
    """-> (headline, what to do): our plain sentence when the code is known, else the core's own words"""
    e = (TEXTS.get(lang) or {}).get("_errors", {}).get(code) or ERRORS.get(code)
    if e:
        return e[0], e[1]
    return message, hint or ""


def step_title(name, title, lang="en"):
    return (TEXTS.get(lang) or {}).get("_steps", {}).get(name, title)
