#!/usr/bin/env python3
"""Compile ntdll's direct-view reservation, commit and release code verbatim,
from the pinned ps5_direct.c and virtual.c with this fork's ps5/wine/patches
applied, against a mocked kernel."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess
import tempfile

DIRECT = 'dlls/ntdll/unix/ps5_direct.c'
VIRTUAL = 'dlls/ntdll/unix/virtual.c'


def load(path, name):
    spec = spec_from_file_location(name, path)
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def span(path, begin, end):
    source = path.read_text()
    if source.count(begin) != 1 or source.count(end) != 1:
        raise SystemExit(f'direct-reserve extraction boundaries missing or ambiguous in {path}')
    return source[source.index(begin):source.index(end)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    switches = load(here / 'build-browser-switches-test.py', 'browser_switches_test')
    surface = load(here / 'build-desktop-surface-test.py', 'surface_test')
    patches = here / '../../wine/patches'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        direct = switches.patched_source(args.wine, patches, work, DIRECT)
        virtual = switches.patched_source(args.wine, patches, work, VIRTUAL)
        # Metadata, bitmaps, reserve, create/free, drop, commit and protect.
        core = span(direct, '#define DIRECT_UNIT_SIZE PS5_KERNEL_DIRECT_ALIGNMENT',
                    'static int direct_read_pools(')
        fallback = '\n\n'.join(surface.definition(virtual, name) for name in (
            'title_direct_reserve_only', 'title_vm_busy', 'title_direct_reserve_fixed',
            'title_direct_reserve', 'title_direct_reserve_high'))
    (args.output.parent / 'wine-direct-reserve-core.inc').write_text(core)
    (args.output.parent / 'wine-direct-reserve-virtual.inc').write_text(fallback + '\n')
    subprocess.run(shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-Wno-unused-variable', '-Wno-sign-compare', '-pthread', '-I' + str(args.output.parent),
        str(here / 'test-direct-reserve.c'), '-o', str(args.output),
    ], check=True)


if __name__ == '__main__':
    main()
