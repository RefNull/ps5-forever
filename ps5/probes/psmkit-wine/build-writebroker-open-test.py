#!/usr/bin/env python3
"""Compile the write broker's write-open path and accept-error policy on the host."""
import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('surface_extraction', here / 'build-desktop-surface-test.py')
    extraction = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(extraction)
    spec = importlib.util.spec_from_file_location(
        'browser_switches_test', here / 'build-browser-switches-test.py')
    switches = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(switches)
    broker = here.parents[1] / 'writebroker/writebroker.c'
    names = ('transfer', 'discard', 'menu_open_child_directory', 'menu_open_prefix_root',
             'menu_open_remove_parent', 'open_user_parent', 'open_user_file', 'serve_write', 'accept_error_action')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    (args.output.parent / 'writebroker-open.inc').write_text(
        '\n\n'.join(extraction.definition(broker, name) for name in names) + '\n')
    with tempfile.TemporaryDirectory() as work:
        subprocess.run(shlex.split(args.cc) + [
            '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-I' + str(switches.patched_include(args.wine, Path(work))),
            '-I' + str(args.wine / 'include'), '-I' + str(args.output.parent),
            str(here / 'test-writebroker-open.c'), '-o', str(args.output),
        ], check=True)
    print('WRITEBROKER_OPEN_ISOLATION ' + ','.join(names) + ' implementation=PRODUCTION_EXTRACTED')


if __name__ == '__main__':
    main()
