"""Parallel parts of the translation: the per-function narrowing plans and the C emission of the functions are
independent of each other (each function's text depends only on the whole-program analysis results, which are
computed before), so they run in a pool of processes. The output is the same text in the same order as the serial
loop (checked byte for byte by the converter's comparison runs and tests/convert/test_parallel_translate.py).

Jobs: PS2RC_TRANSLATE_JOBS (default 1 = the serial loops, unchanged). Each worker holds a copy of the program image,
the function set and the analysis results (a few hundred MB for a large game), so callers cap the job count by
memory (ps2rc.convert sets it).
"""
import importlib
import multiprocessing as mp
import os
import pickle
from concurrent.futures import ProcessPoolExecutor

_CTX = {}
CHUNK = 150


def jobs():
    try:
        return max(1, int(os.environ.get("PS2RC_TRANSLATE_JOBS", "1")))
    except ValueError:
        return 1


def _chunks(seq, n):
    return [seq[k:k + n] for k in range(0, len(seq), n)]


# --- narrowing ------------------------------------------------------------------------------------------------------

def _narrow_init(blob):
    _CTX.clear()
    _CTX.update(pickle.loads(blob))


def _narrow_chunk(addrs):
    from ps2rc.analysis.width import narrowable
    c = _CTX
    return [(a, narrowable(c["img"], c["funcs"][a], c["starts"], c["livein"], c["wide_args"], c["wide_rets"],
                           abi=a not in c["exact"])) for a in addrs]


def narrow_all(img, funcs, starts, livein, wide_args, wide_rets, exact, n):
    """{address: narrowable(...)} for every function, in n processes."""
    blob = pickle.dumps({"img": img, "funcs": funcs, "starts": starts, "livein": livein, "wide_args": wide_args,
                         "wide_rets": wide_rets, "exact": frozenset(exact)}, protocol=pickle.HIGHEST_PROTOCOL)
    plan = {}
    with ProcessPoolExecutor(max_workers=n, initializer=_narrow_init, initargs=(blob,)) as ex:
        for part in ex.map(_narrow_chunk, _chunks(sorted(funcs), CHUNK)):
            plan.update(part)
    return plan


# --- C emission -----------------------------------------------------------------------------------------------------

def _emit_init(blob):
    from ps2rc import backend_c
    _CTX.clear()
    _CTX.update(pickle.loads(blob))
    backend_c.VFLOC_SKIP = _CTX["vfloc_skip"]
    backend_c._LOCAL.clear()
    backend_c._LOCAL.update(_CTX["local"])


def _emit_chunk(addrs):
    from ps2rc import backend_c
    c = _CTX
    out = []
    for a in addrs:
        fn = c["funcs"][a]
        jt = {jr: targets for jr, _table, targets in fn.jump_tables}
        nr = c["narrow"].get(a, frozenset()) if c["narrow"] else frozenset()
        try:
            out.append((a, backend_c.translate_function(c["img"], a, fn.addrs, jt, c["starts"], c["cache"], nr,
                                                        c["fpu_fast"], c["abi"] and a not in c["exempt"], c["livein"],
                                                        program_abi=c["abi"], v5=c["v5"]), None))
        except backend_c.Unsupported as e:
            if not c["stub_unsupported"]:
                raise
            backend_c._LOCAL.update(prefix=c["local"]["prefix"], dispatch=c["local"]["dispatch"],
                                    starts=c["local"]["starts"], external=c["local"]["external"],
                                    exempt=c["local"]["exempt"])
            out.append((a, "void %s(R5900Ctx *c)\n{\n    rt_fatal(c, \"unsupported\", 0x%08xu, 0);\n}\n"
                        % (backend_c.fname(a), a), str(e)))
    return out


def emit_bodies(img, funcs, todo, starts, cache, narrow, fpu_fast, abi, exempt, livein, v5, local, vfloc_skip,
                stub_unsupported, n, progress=None):
    """[(address, C text)] and [(address, message)] of the unsupported ones, like the serial loop of
    backend_c._emit_parts, in n processes."""
    blob = pickle.dumps({"img": img, "funcs": funcs, "starts": starts, "cache": cache, "narrow": narrow,
                         "fpu_fast": fpu_fast, "abi": abi, "exempt": exempt, "livein": livein, "v5": v5,
                         "local": dict(local), "vfloc_skip": vfloc_skip, "stub_unsupported": stub_unsupported},
                        protocol=pickle.HIGHEST_PROTOCOL)
    bodies, unsupported, done = [], [], 0
    with ProcessPoolExecutor(max_workers=n, initializer=_emit_init, initargs=(blob,)) as ex:
        for part in ex.map(_emit_chunk, _chunks(list(todo), CHUNK)):
            for a, text, err in part:
                bodies.append((a, text))
                if err is not None:
                    unsupported.append((a, err))
            done += len(part)
            if progress:
                progress(done, len(todo))
    return bodies, unsupported


# --- whole-program fixpoints: a team of processes, each owning a fixed part of the functions ------------------------

def _team_main(conn, module, init, payload, k, n):
    mod = importlib.import_module(module)
    state = getattr(mod, init)(pickle.loads(payload), k, n)
    conn.send(("ready", None))
    while True:
        msg = conn.recv()
        if msg is None:
            break
        name, args = msg
        try:
            conn.send(("ok", getattr(mod, name)(state, *args)))
        except Exception as e:                                       # reported by the caller
            conn.send(("error", "%s: %s" % (type(e).__name__, e)))
    conn.close()


class Team:
    """n processes; worker k runs module.init(payload, k, n) once (its share of the per-function state, built once)
    and then module.<name>(state, *args) for every round(). Rounds return the n results in worker order; each worker
    keeps its functions for all rounds (a pool would hand chunks to any worker and rebuild their state)."""

    def __init__(self, n, module, init, payload):
        blob = pickle.dumps(payload, protocol=pickle.HIGHEST_PROTOCOL)
        ctx = mp.get_context("spawn")
        self.conns, self.procs = [], []
        for k in range(n):
            a, b = ctx.Pipe()
            pr = ctx.Process(target=_team_main, args=(b, module, init, blob, k, n), daemon=True)
            pr.start()
            b.close()
            self.conns.append(a)
            self.procs.append(pr)
        for c in self.conns:
            self._get(c)

    def _get(self, c):
        kind, val = c.recv()
        if kind == "error":
            self.close()
            raise RuntimeError("translation worker: " + val)
        return val

    def round(self, name, *args):
        for c in self.conns:
            c.send((name, args))
        return [self._get(c) for c in self.conns]

    def close(self):
        for c in self.conns:
            try:
                c.send(None)
                c.close()
            except OSError:
                pass
        for pr in self.procs:
            pr.join(30)
        self.conns, self.procs = [], []
