"""The mouse wheel never changes a value by accident: over a drop-down, a spin box or a slider that does not have the
keyboard focus, the wheel scrolls the page around it instead (settings were changed by accident while scrolling). Click the
control (or Tab to it) first, then the wheel changes it. One application-wide event filter covers every window,
dialog and page, present and future."""
from PySide6.QtCore import QCoreApplication, QEvent, QObject, Qt
from PySide6.QtWidgets import QAbstractScrollArea, QAbstractSlider, QAbstractSpinBox, QComboBox, QScrollBar


def guarded(w):
    """a control whose value the wheel would change"""
    return isinstance(w, (QComboBox, QAbstractSpinBox)) or (isinstance(w, QAbstractSlider)
                                                           and not isinstance(w, QScrollBar))


class WheelGuard(QObject):
    def eventFilter(self, obj, ev):
        t = ev.type()
        if t == QEvent.Polish and guarded(obj):
            obj.setFocusPolicy(Qt.StrongFocus)              # the wheel alone never gives them the focus
        elif t == QEvent.Wheel and guarded(obj) and not obj.hasFocus():
            p = obj.parentWidget()
            while p is not None and not isinstance(p, QAbstractScrollArea):
                p = p.parentWidget()
            if p is not None:                               # scroll the page the control sits in
                bar = p.verticalScrollBar() if ev.angleDelta().y() else p.horizontalScrollBar()
                QCoreApplication.sendEvent(bar, ev)
            return True                                     # the control's value stays as it is
        return False


_guard = None


def install(app):
    """once per QApplication"""
    global _guard
    if _guard is None:
        _guard = WheelGuard(app)
        app.installEventFilter(_guard)
    return _guard
