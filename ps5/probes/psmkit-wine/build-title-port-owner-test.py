#!/usr/bin/env python3
"""Build the title's write-broker probe and add-on worker port wait on the host."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess

NAMES = ('pw_writebroker_transfer', 'pw_writebroker_probe', 'pw_addon_worker_get_u32',
         'pw_addon_worker_put_u32', 'pw_addon_worker_answers', 'pw_addon_worker_wait_stopped')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--native-entry', type=Path,
                        help='title source (default: native-entry.c beside this script)')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    source = args.native_entry or here / 'native-entry.c'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    (args.output.parent / 'title-port-owner.inc').write_text(
        '\n\n'.join(module.definition(source, name) for name in NAMES) + '\n')
    subprocess.run(shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(args.wine / 'include'), '-I' + str(args.output.parent),
        str(here / 'test-title-port-owner.c'), '-o', str(args.output),
    ], check=True)
    print('TITLE_PORT_OWNER_ISOLATION ' + ','.join(NAMES) + ' implementation=PRODUCTION_EXTRACTED')


if __name__ == '__main__':
    main()
