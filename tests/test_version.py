# tools/version.py: single source of truth from pyproject.toml + v* git tags.
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import version  # noqa: E402


def _git(repo, *args):
    subprocess.run(['git', *args], cwd=repo, check=True, capture_output=True, text=True)


def _init_repo(tmp_path, pkg_version='0.2.3'):
    (tmp_path / 'pyproject.toml').write_text(f'[project]\nname = "x"\nversion = "{pkg_version}"\n')
    _git(tmp_path, 'init')
    _git(tmp_path, 'config', 'user.email', 'test@example.com')
    _git(tmp_path, 'config', 'user.name', 'Test')
    _git(tmp_path, 'add', 'pyproject.toml')
    _git(tmp_path, 'commit', '-m', 'init')
    return _git_sha(tmp_path)


def _git_sha(repo):
    return subprocess.run(
        ['git', 'rev-parse', '--short', 'HEAD'], cwd=repo, check=True, capture_output=True, text=True
    ).stdout.strip()


def test_untagged_clean_and_dirty(tmp_path):
    sha = _init_repo(tmp_path, '0.2.3')
    assert version.resolve_version(tmp_path) == f'v0.2.3-dev+{sha}'
    (tmp_path / 'extra.txt').write_text('dirty')
    assert version.resolve_version(tmp_path) == f'v0.2.3-dev+{sha}-dirty'


def test_exact_tag_clean_and_dirty(tmp_path):
    sha = _init_repo(tmp_path, '0.2.3')
    _git(tmp_path, 'tag', 'v0.2.3')
    assert version.resolve_version(tmp_path) == 'v0.2.3'
    (tmp_path / 'extra.txt').write_text('dirty')
    assert version.resolve_version(tmp_path) == f'v0.2.3-dev+{sha}-dirty'


def test_prerelease_tag(tmp_path):
    _init_repo(tmp_path, '0.2.3')
    _git(tmp_path, 'tag', 'v0.2.3-rc1')
    assert version.resolve_version(tmp_path) == 'v0.2.3-rc1'


def test_check_tag(tmp_path):
    (tmp_path / 'pyproject.toml').write_text('[project]\nname = "x"\nversion = "0.2.3"\n')
    assert version.check_tag('v0.2.3', tmp_path)[0]
    assert version.check_tag('v0.2.3-rc1', tmp_path)[0]
    assert not version.check_tag('0.2.3', tmp_path)[0]
    assert not version.check_tag('v0.2.4', tmp_path)[0]
