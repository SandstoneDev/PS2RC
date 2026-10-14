"""The environment doctor and, where the checkout has the agent kit, its agent skills and knowledge base of field notes.

Everything here reads only the checkout and the machine's tool installation; no game data, no network."""
import os

from ps2rc import config


def repo_root():
    return config.repo_root()


def find_dir(name):
    """<repo>/<name>; in the development worktree the public documents live under docs/public/ instead."""
    root = repo_root()
    for d in (os.path.join(root, name), os.path.join(root, "docs", "public", name)):
        if os.path.isdir(d):
            return d
    return os.path.join(root, name)


def find_file(name):
    """<repo>/<name>, else docs/public/<name> (development worktree)."""
    root = repo_root()
    for p in (os.path.join(root, name), os.path.join(root, "docs", "public", name)):
        if os.path.isfile(p):
            return p
    return None
