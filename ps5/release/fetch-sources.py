#!/usr/bin/env python3
"""Check out the release's public dependency pins in SOURCE_ROOT.

Run from the product checkout already at SOURCE_ROOT/ps5-proton/ps5-wow:
    python3 ps5/release/fetch-sources.py SOURCE_ROOT
No recursive Proton checkout or private build history is needed.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path, PurePosixPath


def git(path, *arguments, capture=False):
    result = subprocess.run(['git', '-C', str(path), *arguments], check=True,
                            stdout=subprocess.PIPE if capture else None, text=True)
    return result.stdout.strip() if capture else None


def restore_pinned_files(path):
    """Rewrite only the tracked files whose content differs from the index.

    `git checkout -- .` also rewrites files that are merely stat-dirty (same
    content, newer mtime or another inode). A Wine cache miss touches every
    source file, so a plain checkout on the next run gave all of them a new
    mtime and made Wine's incremental make rebuild the whole tree."""
    subprocess.run(['git', '-C', str(path), 'update-index', '-q', '--refresh'],
                   stdout=subprocess.DEVNULL)
    git(path, 'checkout', '--', '.')


def restore_patched(path):
    """Undo a patch series a previous build applied in place; True if one was."""
    stamp = Path(git(path, 'rev-parse', '--path-format=absolute',
                     '--git-path', 'forever-patches', capture=True))
    if not stamp.is_file():
        return False
    restore_pinned_files(path)
    stamp.unlink()
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_root', type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    manifest = json.loads(Path(__file__).with_name('deps.json').read_text())
    # Parent repositories must exist before their nested submodule checkouts.
    for source in sorted(manifest['sources'], key=lambda entry: len(PurePosixPath(entry['path']).parts)):
        relative = PurePosixPath(source['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise SystemExit(f'unsafe dependency path: {relative}')
        destination = root / relative
        if not destination.resolve().is_relative_to(root):
            raise SystemExit(f'dependency path escapes source root: {relative}')
        revision = source['commit']
        url = source['url']
        if not re.fullmatch(r'[0-9a-f]{40}', revision):
            raise SystemExit(f"{source['name']}: expected a full commit ID")
        if not url.startswith('https://github.com/') or '@' in url:
            raise SystemExit(f"{source['name']}: expected a public GitHub HTTPS URL")
        if (destination / '.git').exists():
            if git(destination, 'rev-parse', 'HEAD', capture=True) != revision:
                raise SystemExit(f'{relative}: existing checkout is not at {revision}')
            # A previous build applied this repository's patch series in place
            # (ps5/build-wine.sh, ps5/build-vkd3d-proton.sh) and left a stamp.
            # Restore the pinned files so validate_sources sees a clean tree;
            # changes without a stamp are not ours and still fail there.
            if restore_patched(destination):
                print(f'RESTORED {source["name"]} pinned files after a patched build', flush=True)
            print(f'CHECKOUT {source["name"]} {revision} existing', flush=True)
            continue
        if destination.exists() and any(destination.iterdir()):
            raise SystemExit(f'{relative}: nonempty directory is not a dependency checkout')
        destination.mkdir(parents=True, exist_ok=True)
        git(destination, 'init', '--quiet')
        git(destination, 'remote', 'add', 'origin', url)
        git(destination, 'fetch', '--quiet', '--depth=1', 'origin', revision)
        git(destination, '-c', 'advice.detachedHead=false', 'checkout', '--quiet', '--detach', 'FETCH_HEAD')
        if git(destination, 'rev-parse', 'HEAD', capture=True) != revision:
            raise SystemExit(f'{relative}: fetched revision does not match the pin')
        print(f'CHECKOUT {source["name"]} {revision}', flush=True)


if __name__ == '__main__':
    main()
