#!/usr/bin/env python3
"""Resolve the relay version from git tags and pyproject.toml.

On a clean commit matching an exact `v*` tag, prints that tag (e.g. `v0.1.0` or
`v0.1.0-rc1`). Otherwise reads `[project].version` from `pyproject.toml` and
appends `-dev+<sha>` (and `-dirty` when the working tree has changes).
"""

import argparse
import pathlib
import re
import subprocess
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.10
    tomllib = None

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read_package_version(root=ROOT):
    pyproject = pathlib.Path(root) / 'pyproject.toml'
    text = pyproject.read_text(encoding='utf-8')
    if tomllib is not None:
        return str(tomllib.loads(text)['project']['version'])
    in_project = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('['):
            in_project = s == '[project]'
        elif in_project:
            m = re.match(r'^version\s*=\s*["\']([^"\']+)["\']', s)
            if m:
                return m.group(1)
    raise ValueError(f'missing [project].version in {pyproject}')


def _git(root, *args):
    return subprocess.run(['git', *args], cwd=root, capture_output=True, text=True)


def is_dirty(root=ROOT):
    r = _git(root, 'status', '--porcelain')
    return r.returncode == 0 and bool(r.stdout.strip())


def exact_v_tag(root=ROOT):
    r = _git(root, 'describe', '--tags', '--exact-match', '--match', 'v*', 'HEAD')
    if r.returncode == 0:
        return r.stdout.strip() or None
    return None


def short_sha(root=ROOT):
    r = _git(root, 'rev-parse', '--short', 'HEAD')
    if r.returncode == 0:
        return r.stdout.strip() or None
    return None


def resolve_version(root=ROOT):
    dirty = is_dirty(root)
    tag = exact_v_tag(root)
    if tag and not dirty:
        return tag
    pkg = read_package_version(root)
    sha = short_sha(root)
    suffix = '-dirty' if dirty else ''
    return f'v{pkg}-dev+{sha}{suffix}' if sha else f'v{pkg}-dev{suffix}'


def check_tag(tag, root=ROOT):
    pkg = read_package_version(root)
    if not tag.startswith('v'):
        return False, f'tag {tag!r} must start with "v"'
    base = tag[1:].split('-', 1)[0]
    if base != pkg:
        return False, f'tag {tag} (base {base}) does not match pyproject.toml version {pkg}'
    return True, f'tag {tag} matches pyproject.toml version {pkg}'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', type=pathlib.Path, default=ROOT, help='repository root')
    ap.add_argument('--package', action='store_true', help='print [project].version from pyproject.toml')
    ap.add_argument('--check-tag', metavar='TAG', help='verify a v* tag matches pyproject.toml')
    a = ap.parse_args(argv)
    if a.check_tag is not None:
        ok, msg = check_tag(a.check_tag, a.root)
        if not ok:
            print(msg, file=sys.stderr)
            return 1
        print(msg)
        return 0
    print(read_package_version(a.root) if a.package else resolve_version(a.root))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
