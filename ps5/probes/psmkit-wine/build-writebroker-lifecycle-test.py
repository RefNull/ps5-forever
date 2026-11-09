#!/usr/bin/env python3
"""Compile the whole write broker, main() included, into a host lifecycle test.

Only the console log (ps5/klog.h) is a host stub; the test runs real broker
processes against a loopback port and a scratch user data folder.
"""
import argparse
from pathlib import Path
import shlex
import subprocess

MENU_INCLUDE = '#include "../probes/psmkit-wine/writebroker-menu.h"'
KLOG_STUB = '''#ifndef TEST_PS5_KLOG_H
#define TEST_PS5_KLOG_H
int klog_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--broker', type=Path,
                        help='broker source (default: ps5/writebroker/writebroker.c)')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    broker = args.broker or here.parents[1] / 'writebroker/writebroker.c'
    source = broker.read_text()
    if source.count(MENU_INCLUDE) != 1:
        raise SystemExit(f'cannot find the menu header include in {broker}')
    out = args.output.parent
    (out / 'writebroker-lifecycle-stub/ps5').mkdir(parents=True, exist_ok=True)
    (out / 'writebroker-lifecycle-stub/ps5/klog.h').write_text(KLOG_STUB)
    (out / 'writebroker-lifecycle.inc').write_text(source.replace(
        MENU_INCLUDE, '#include "%s"' % (here / 'writebroker-menu.h')))
    subprocess.run(shlex.split(args.cc) + [
        '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(args.wine / 'include'), '-I' + str(out),
        '-I' + str(out / 'writebroker-lifecycle-stub'),
        str(here / 'test-writebroker-lifecycle.c'), '-o', str(args.output),
    ], check=True)
    print('WRITEBROKER_LIFECYCLE_ISOLATION whole=' + str(broker.name)
          + ' implementation=PRODUCTION_SOURCE')


if __name__ == '__main__':
    main()
