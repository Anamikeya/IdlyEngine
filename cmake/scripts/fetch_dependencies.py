#!/usr/bin/env python3
"""Check out third-party dependencies at the commits pinned in cmake/DependencyLock.cmake.

Used by CI to recreate GAMEENGINE_RESOURCES_DIR. Developers normally point GAMEENGINE_RESOURCES_DIR
at the shared, already-audited checkout instead of running this.

    python cmake/scripts/fetch_dependencies.py <destination> box3d GameNetworkingSockets
    python cmake/scripts/fetch_dependencies.py <destination> --all
"""

import argparse
import pathlib
import re
import subprocess
import sys

LOCK_FILE = pathlib.Path(__file__).resolve().parents[1] / "DependencyLock.cmake"
ENTRY = re.compile(r'"([^|"]+)\|([^|"]+)\|([0-9a-f]{40})"')


def read_lock():
    entries = {}
    for name, url, commit in ENTRY.findall(LOCK_FILE.read_text(encoding="utf-8")):
        entries[name] = (url, commit)
    if not entries:
        sys.exit(f"no entries parsed from {LOCK_FILE}")
    return entries


def git(*args, cwd):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def head(path):
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=path, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def fetch(destination, name, url, commit):
    target = destination / name
    if target.exists():
        current = head(target)
        if current == commit:
            print(f"{name}: already at {commit}")
            return
        sys.exit(f"{name}: {target} exists at {current}, expected {commit}; refusing to modify it")
    target.mkdir(parents=True)
    git("init", "--quiet", cwd=target)
    git("remote", "add", "origin", url, cwd=target)
    # GitHub serves reachable commits by id, so a shallow fetch of exactly the locked commit works.
    git("fetch", "--quiet", "--depth", "1", "origin", commit, cwd=target)
    git("checkout", "--quiet", "FETCH_HEAD", cwd=target)
    actual = head(target)
    if actual != commit:
        sys.exit(f"{name}: checked out {actual}, expected {commit}")
    print(f"{name}: {commit}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("destination", type=pathlib.Path)
    parser.add_argument("names", nargs="*")
    parser.add_argument("--all", action="store_true", help="fetch every locked dependency")
    args = parser.parse_args()

    lock = read_lock()
    names = list(lock) if args.all else args.names
    if not names:
        parser.error("name at least one dependency or pass --all")
    unknown = [n for n in names if n not in lock]
    if unknown:
        sys.exit(f"not in lock file: {', '.join(unknown)}")

    args.destination.mkdir(parents=True, exist_ok=True)
    for name in names:
        url, commit = lock[name]
        fetch(args.destination.resolve(), name, url, commit)


if __name__ == "__main__":
    main()
