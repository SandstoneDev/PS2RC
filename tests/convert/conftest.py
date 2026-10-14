"""Converter tests: everything is generated here (synthetic disc images, synthetic game packs); no game data."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

# The converter lives in <repo>/ps2rc/convert; while the core package is still imported from src/ (pytest.ini), add
# the repository's ps2rc folder to the package path so ps2rc.convert (and ps2rc.pbp) resolve either way.
try:
    import ps2rc.convert  # noqa: F401
except ImportError:
    if ROOT not in sys.path:
        sys.path.insert(0, ROOT)
    import ps2rc
    _p = os.path.join(ROOT, "ps2rc")
    if _p not in list(ps2rc.__path__):
        ps2rc.__path__.append(_p)
    import ps2rc.convert  # noqa: F401,E402
