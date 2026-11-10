#!/usr/bin/env python3
"""Compile ntdll's browser-switch functions verbatim, from the pinned process.c
with this fork's ps5/wine/patches applied, in a host harness."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

SOURCE = 'dlls/ntdll/unix/process.c'


def patched_source(wine, patches, work):
    """process.c with every patch that touches it, whether or not the checkout
    already carries them (build-wine.sh patch runs after the host tests)."""
    (work / SOURCE).parent.mkdir(parents=True)
    shutil.copy2(wine / SOURCE, work / SOURCE)
    for patch in sorted(patches.glob('*.patch')):
        if f'diff --git a/{SOURCE} ' not in patch.read_text():
            continue
        apply = ['git', 'apply', f'--include={SOURCE}', str(patch)]
        if subprocess.run(apply + ['--check'], cwd=work, capture_output=True).returncode == 0:
            subprocess.run(apply, cwd=work, check=True)
        elif subprocess.run(apply + ['--check', '--reverse'], cwd=work,
                            capture_output=True).returncode != 0:
            raise SystemExit(f'{patch.name} neither applies to nor is applied in {wine / SOURCE}')
    return work / SOURCE


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
