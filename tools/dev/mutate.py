"""Mutation check: apply one textual mutation to a source file, run a pytest selection, restore the file.
Exit code 0 when the mutation was caught (tests failed), 1 when it survived.

    python tools/dev/mutate.py <file> <old> <new> <pytest args...>
"""
import shutil
import subprocess
import sys


def main():
    path, old, new, *pytest_args = sys.argv[1:]
    src = open(path, encoding="utf-8").read()
    if src.count(old) != 1:
        print("mutation target must occur exactly once (found %d): %r" % (src.count(old), old))
        return 2
    backup = path + ".mutbak"
    shutil.copyfile(path, backup)
    try:
        open(path, "w", encoding="utf-8", newline="\n").write(src.replace(old, new))
        p = subprocess.run([sys.executable, "-m", "pytest", "-x", "-q", *pytest_args], capture_output=True, text=True)
    finally:
        shutil.copyfile(backup, path)
        import os
        os.remove(backup)
    caught = p.returncode != 0
    print("CAUGHT" if caught else "SURVIVED", repr(old), "->", repr(new))
    if not caught:
        print(p.stdout[-2000:])
    return 0 if caught else 1


if __name__ == "__main__":
    sys.exit(main())
