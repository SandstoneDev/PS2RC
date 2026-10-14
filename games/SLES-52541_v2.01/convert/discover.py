"""Conversion step: function discovery of the boot program (main code + static-constructor code).

    python convert/discover.py

Writes the pack's [translate] functions file (a pickled dict start -> Function) into the work folder."""
import os
import pickle
import time

from common import FUNCTIONS, discover_functions, main_image, say


def main():
    os.makedirs(os.path.dirname(FUNCTIONS), exist_ok=True)
    img, code = main_image()
    t = time.time()
    funcs = discover_functions(img, code).functions
    with open(FUNCTIONS, "wb") as f:
        pickle.dump(funcs, f)
    say("main code %s: %d functions in %.0f s -> %s" % (["%08x-%08x" % r for r in code], len(funcs), time.time() - t,
                                                       FUNCTIONS))


if __name__ == "__main__":
    main()
