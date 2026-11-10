#!/usr/bin/env python3
"""Compile the whole write broker and the in-process server's namespace client
(server/fd.c, with this fork's ps5/wine/patches applied) into one host test.

The broker runs as a real process on a loopback port over a scratch user data
folder; the client is the production block between the fork's markers.
"""
import argparse
import importlib.util
from pathlib import Path
import shlex
import subprocess
import tempfile

BEGIN = '/* WINE_PS5_NAMESPACE_BROKER_BEGIN */'
END = '/* WINE_PS5_NAMESPACE_BROKER_END */'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--wine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location(
        'lifecycle_build', here / 'build-writebroker-lifecycle-test.py')
    lifecycle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifecycle)
    spec = importlib.util.spec_from_file_location(
        'browser_switches_test', here / 'build-browser-switches-test.py')
    switches = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(switches)

    broker = here.parents[1] / 'writebroker/writebroker.c'
    source = broker.read_text()
    if source.count(lifecycle.MENU_INCLUDE) != 1:
        raise SystemExit(f'cannot find the menu header include in {broker}')

    out = args.output.parent
    (out / 'writebroker-namespace-stub/ps5').mkdir(parents=True, exist_ok=True)
    (out / 'writebroker-namespace-stub/ps5/klog.h').write_text(lifecycle.KLOG_STUB)
    (out / 'writebroker-namespace.inc').write_text(source.replace(
        lifecycle.MENU_INCLUDE, '#include "%s"' % (here / 'writebroker-menu.h')))
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        server = switches.patched_source(args.wine, switches.PATCHES, work, 'server/fd.c').read_text()
        if server.count(BEGIN) != 1 or server.count(END) != 1:
            raise SystemExit('namespace client extraction markers missing or ambiguous in server/fd.c')
        (out / 'wine-namespace.inc').write_text(
            server[server.index(BEGIN) + len(BEGIN):server.index(END)])
        subprocess.run(shlex.split(args.cc) + [
            '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-I' + str(switches.patched_include(args.wine, work)),
            '-I' + str(args.wine / 'include'), '-I' + str(out),
            '-I' + str(out / 'writebroker-namespace-stub'),
            str(here / 'test-writebroker-namespace.c'), '-o', str(args.output),
        ], check=True)
    print('WRITEBROKER_NAMESPACE_ISOLATION broker=' + broker.name
          + ' client=server/fd.c implementation=PRODUCTION_SOURCE')


if __name__ == '__main__':
    main()
