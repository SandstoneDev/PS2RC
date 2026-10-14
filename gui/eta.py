"""Time left of a conversion, from the converter's own per-step estimate (ps2rc.convert.estimate) corrected by how
fast this PC has been so far. Pure: fed with the progress events, asked for seconds left."""
import time

# reference seconds per step when no estimate is known yet (measured app-folder run, 2026-10-13, 384 s in total)
DEFAULT = {"identify": 2, "extract": 5, "analysis": 120, "translate": 150, "extras": 20, "modules": 2,
           "prepare": 15, "compile": 50, "link": 3, "prx": 3, "eboot": 3, "stage": 15, "movies": 10, "verify": 10}


class Eta:
    def __init__(self, steps, weights=None, clock=time.time):
        self.steps = list(steps)                 # step names in order
        self.w = dict(DEFAULT)
        self.w.update(weights or {})
        self.clock = clock
        self.cur, self.cur_t0, self.frac = None, None, 0.0
        self.done = {}                            # name -> (seconds, cached)
        self.t0 = clock()

    def set_estimate(self, steps_seconds):
        """the converter's estimate: {'analysis': s, 'translate': s, 'compile': s (incl. link), ...}"""
        for k, v in (steps_seconds or {}).items():
            if k in self.w and v is not None:
                self.w[k] = max(1, float(v))
        if "compile" in (steps_seconds or {}):
            self.w["link"] = 1

    def step(self, name):
        self.cur, self.cur_t0, self.frac = name, self.clock(), 0.0

    def finish(self, name, seconds, cached):
        self.done[name] = (seconds, cached)
        if self.cur == name:
            self.cur = None

    def pct(self, name, done, total):
        if name == self.cur and total:
            self.frac = max(0.0, min(1.0, done / float(total)))

    def speed(self):
        """actual / estimated over the finished, really-run steps (1.0 until something ran); clamped 0.3..4"""
        est = act = 0.0
        for n, (s, cached) in self.done.items():
            if not cached and self.w.get(n, 0) >= 5:
                est += self.w[n]
                act += s
        return min(4.0, max(0.3, act / est)) if est > 0 else 1.0

    def left(self):
        """seconds left (None before the first step)"""
        if self.cur is None and not self.done:
            return None
        k = self.speed()
        rest = 0.0
        for n in self.steps:
            if n in self.done:
                continue
            if n == self.cur:
                spent = self.clock() - (self.cur_t0 or self.clock())
                guess = self.w.get(n, 5) * k
                if self.frac > 0.02:
                    guess = spent / self.frac        # the step's own progress is the best guide
                rest += max(0.0, guess - spent)
                continue
            rest += self.w.get(n, 5) * k
        return rest

    def fraction(self):
        total = sum(self.w.get(n, 5) for n in self.steps)
        got = sum(self.w.get(n, 5) for n in self.steps if n in self.done)
        if self.cur:
            got += self.w.get(self.cur, 5) * self.frac
        return got / total if total else 0.0


def fmt_left(seconds, lang="en"):
    if seconds is None:
        return ""
    import i18n
    if seconds < 60:
        return i18n.tr("eta.lt1", lang)
    return i18n.tr("eta.min", lang, int(round(seconds / 60.0)))
