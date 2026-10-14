"""IOP recompiler tests: synthetic modules only (no game data). The package lives in <repo>/ps2rc/iop; while the rest
of ps2rc is still imported from src/ (pytest.ini), add the repository's ps2rc folder to the package path."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
try:
    import ps2rc.iop  # noqa: F401
except ImportError:
    if ROOT not in sys.path:
        sys.path.insert(0, ROOT)
    import ps2rc
    _p = os.path.join(ROOT, "ps2rc")
    if _p not in list(ps2rc.__path__):
        ps2rc.__path__.append(_p)
    import ps2rc.iop  # noqa: F401,E402
