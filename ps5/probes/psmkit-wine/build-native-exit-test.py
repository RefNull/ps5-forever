#!/usr/bin/env python3
"""Build the production title exit (pw_native_exit) on the host against stubs."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = spec_from_file_location('surface_test', here / 'build-desktop-surface-test.py')
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    (args.output.parent / 'native-exit-function.inc').write_text(
        module.definition(here / 'native-entry.c', 'pw_native_exit') + '\n')
    command = shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(args.output.parent), str(here / 'test-native-exit.c'),
        '-o', str(args.output)]
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
