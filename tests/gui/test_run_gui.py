"""gui/run_gui.py: Qt's plugin folders come from the installed PySide6, Qt settings from elsewhere are not inherited,
and a window that cannot start leaves a log and a plain message. No test starts a real window or a Qt failure: a
failing Qt start would put a modal box on the desktop."""
import os

import pytest

pytest.importorskip("PySide6")
import run_gui  # noqa: E402

FAKE_GUI = r'''
import os, sys
sys.stderr.write("qt QtFatalMsg: no Qt platform plugin could be initialized (%s) debug=%s\n"
                 % (sys.argv[1:], os.environ.get("QT_DEBUG_PLUGINS")))
sys.exit(3)
'''


def test_environment_is_clean_and_holds_no_qt_paths(monkeypatch):
    """the window gets no Qt plugin paths through the environment (they would reach every program it starts)"""
    monkeypatch.setenv("QT_QPA_PLATFORM", "offscreen")
    monkeypatch.setenv("QT_PLUGIN_PATH", os.path.join("X:" + os.sep, "elsewhere"))
    env, pdir = run_gui.qt_env()
    assert "QT_QPA_PLATFORM" not in env and pdir and os.path.isdir(pdir)
    assert "QT_PLUGIN_PATH" not in env and "QT_QPA_PLATFORM_PLUGIN_PATH" not in env
    assert env.get("PATH") == os.environ.get("PATH")
    assert run_gui.qt_env(debug=True)[0]["QT_DEBUG_PLUGINS"] == "1"


def test_children_get_no_console_and_no_app_environment(monkeypatch, tmp_path):
    import subprocess
    import sys
    from ps2rc import proc
    if os.name == "nt":
        kw = proc.hidden()
        assert kw["creationflags"] & 0x08000000 and kw["startupinfo"].wShowWindow == 0
    own = os.path.dirname(sys.executable)
    monkeypatch.setenv("QT_PLUGIN_PATH", own)
    monkeypatch.setenv("QT_QPA_PLATFORM_PLUGIN_PATH", own)
    monkeypatch.setenv("PYTHONPATH", own)
    monkeypatch.setenv("PATH", os.pathsep.join([own, str(tmp_path)]))
    env = proc.clean_env()
    assert not [k for k in env if k.upper().startswith("QT_") or k.upper() == "PYTHONPATH"]
    assert env["PATH"] == str(tmp_path)
    for k in [k for k in os.environ if k.upper().startswith(("QT_", "PYTHON"))]:
        monkeypatch.setenv(k, os.environ[k])            # restored after the test (offscreen Qt stays)
    gone = proc.scrub_environment()                     # the window does this to itself after Qt started
    assert "QT_PLUGIN_PATH" in gone and "QT_PLUGIN_PATH" not in os.environ and os.environ["PATH"] == str(tmp_path)
    # a program started afterwards (here: a stand-in for an emulator Windows might start) sees none of it
    out = subprocess.run([sys.executable, "-c", "import os; print(os.environ.get('QT_PLUGIN_PATH'))"],
                         capture_output=True, **proc.hidden()).stdout.decode().strip()
    assert out == "None"
    if os.name == "nt":
        exe = os.path.join(own, "pythonw.exe")
        monkeypatch.setattr(sys, "executable", exe)
        if os.path.isfile(exe) and os.path.isfile(os.path.join(own, "python.exe")):
            assert proc.python_for_children().lower().endswith("python.exe")


def test_failed_start_logs_and_tells(tmp_path, monkeypatch):
    """a stand-in window that fails like Qt without a platform plugin"""
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    fake = tmp_path / "fake_gui.py"
    fake.write_text(FAKE_GUI)
    monkeypatch.setattr(run_gui, "GUI", str(fake))
    said = []
    monkeypatch.setattr(run_gui, "message", lambda text, title="": said.append(text))
    code = run_gui.main(["-platform", "no_such_platform"])
    log = tmp_path / "PS2RC" / "gui_start.log"
    text = log.read_text(encoding="utf-8", errors="replace")
    assert code == 3 and said and str(log) in said[0]
    assert "no_such_platform" in text and "QT_DEBUG_PLUGINS=1" in text and "debug=1" in text   # verbose 2nd start


def test_qt_messages_go_to_stderr(capsys):
    """quiet_qt(): Qt's own messages are written to stderr (the start log), not shown in a box"""
    import ps2rc_gui
    from PySide6.QtCore import qInstallMessageHandler, qWarning
    ps2rc_gui.quiet_qt()
    try:
        qWarning("probe-123")
        assert "probe-123" in capsys.readouterr().err
    finally:
        qInstallMessageHandler(None)
