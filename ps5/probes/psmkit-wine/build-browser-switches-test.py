#!/usr/bin/env python3
"""Compile ntdll's browser-switch functions verbatim, from the pinned process.c
with this fork's ps5/wine/patches applied, in a host harness."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

SOURCE = 'dlls/ntdll/unix/process.c'
PATCHES = Path(__file__).resolve().parents[2] / 'wine/patches'


def patched_source(wine, patches, work, source=SOURCE):
    """source (process.c by default) with every patch that touches it, whether
    or not the checkout already carries them (build-wine.sh patch runs after
    the host tests)."""
    (work / source).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(wine / source, work / source)
    for patch in sorted(patches.glob('*.patch')):
        if f'diff --git a/{source} ' not in patch.read_text():
            continue
        apply = ['git', 'apply', f'--include={source}', str(patch)]
        if subprocess.run(apply + ['--check'], cwd=work, capture_output=True).returncode == 0:
            subprocess.run(apply, cwd=work, check=True)
        elif subprocess.run(apply + ['--check', '--reverse'], cwd=work,
                            capture_output=True).returncode != 0:
            raise SystemExit(f'{patch.name} neither applies to nor is applied in {wine / source}')
    return work / source


def patched_include(wine, work, patches=PATCHES):
    """work/include holding each fork header the patches change, patched; put
    it before wine/include on the include path."""
    (work / 'include').mkdir(parents=True, exist_ok=True)
    for patch in sorted(patches.glob('*.patch')):
        for header in re.findall(r'^diff --git a/(include/\S+) ', patch.read_text(), re.M):
            if not (work / header).exists():
                patched_source(wine, patches, work, header)
    return work / 'include'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        source = patched_source(args.wine, here / '../../wine/patches', Path(directory))
        functions = '\n\n'.join(module.definition(source, name) for name in (
            'wcs_has_ascii', 'title_feature_switch', 'title_switch_end', 'title_merge_features',
            'title_browser_switches'))
    (args.output.parent / 'browser-switches.inc').write_text(functions + '\n')
    command = shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-I' + str(args.output.parent), str(here / 'test-browser-switches.c'), '-o', str(args.output)]
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
