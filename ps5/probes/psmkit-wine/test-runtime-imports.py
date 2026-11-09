#!/usr/bin/env python3
"""Host semantic tests for deny-by-default title import ownership."""
import argparse
import copy
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

from runtime_imports import (
    candidate_ownership, load_policy, make_records, module_names, module_paths, render_include,
)

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
policy_path = here / 'runtime-import-policy-b1.json'
reviewed = load_policy(policy_path)
# A launcher build: the policy's GPU-only module rows are not its authority.
policy = [row for row in reviewed if row['module'] in module_names()]

module_symbols = {module: {'strong': set(), 'weak': set()} for module in module_names()}
for row in policy:
    module_symbols[row['module']][row['binding']].add(row['symbol'])
records = make_records(module_symbols, reviewed)
if len(records) != len({row['symbol'] for row in policy}):
    raise SystemExit('reviewed policy did not merge deterministically')

expected_mappings = {
    'dlopen': ('custom_loader', 'pw_public_dlopen'),
    'dlsym': ('custom_loader', 'pw_public_dlsym'),
    'dlclose': ('custom_loader', 'pw_public_dlclose'),
    'dlerror': ('custom_loader', 'pw_public_dlerror'),
    'dladdr': ('custom_loader', 'pw_public_dladdr'),
    '__real_pthread_key_create': ('native_pthread', 'pthread_key_create'),
    'syscall': ('fail_closed', 'pw_native_fail_syscall'),
    'fork': ('fail_closed', 'pw_native_fail_fork'),
    'setsid': ('fail_closed', 'pw_native_fail_setsid'),
    '__tls_get_addr': ('custom_tls', 'pw_native_tls_get_addr'),
    'extattr_get_fd': ('fail_closed', 'pw_native_fail_minus_one'),
    'dup': ('custom_fd', 'pw_native_dup_fd'),
    'posix_fadvise': ('fail_closed', 'pw_native_fail_posix_error'),
    'if_nametoindex': ('fail_closed', 'pw_native_fail_if_nametoindex'),
    'getmntinfo': ('fail_closed', 'pw_native_fail_getmntinfo'),
    'futimens': ('platform', 'ps5_futimens'),
    'posix_fallocate': ('platform', 'ps5_posix_fallocate'),
    'realpath': ('platform', 'ps5_realpath'),
    'umask': ('platform', 'ps5_umask'),
    'isatty': ('platform', 'ps5_isatty'),
    'link': ('platform', 'ps5_link'),
    'execv': ('fail_closed', 'pw_native_fail_minus_one'),
    'fchdir': ('fail_closed', 'pw_native_fail_minus_one'),
    'openat': ('platform', 'ps5_openat'),
    'fstatat': ('platform', 'ps5_fstatat'),
    'fstatfs': ('platform', 'ps5_fstatfs'),
    'readlink': ('platform', 'ps5_readlink'),
    'symlink': ('platform', 'ps5_symlink'),
}
for symbol, expected in expected_mappings.items():
    if candidate_ownership(symbol) != expected:
        raise SystemExit(f'explicit ownership mapping drift for {symbol}')
if candidate_ownership('future_secret_runtime_api') != (
        'UNREVIEWED_NOT_AUTHORIZED', 'future_secret_runtime_api'):
    raise SystemExit('unknown import received permissive candidate ownership')

for symbol in ('NtGdiExtGetObjectW', 'NtGdiGetDIBitsInternal', 'NtUserGetIconInfo'):
    if candidate_ownership(symbol) != ('title_win32u', symbol):
        raise SystemExit(f'cursor GDI import is not owned by title win32u: {symbol}')
    row = next((row for row in reviewed
                if row['module'] == 'wineps5.so' and row['symbol'] == symbol), None)
    if not row or row['owner'] != 'title_win32u' or row['target'] != symbol:
        raise SystemExit(f'cursor GDI import missing from reviewed B1 policy: {symbol}')

driver_tls = next((row for row in reviewed
                   if row['module'] == 'wineps5.so' and row['symbol'] == '__tls_get_addr'), None)
if driver_tls:
    raise SystemExit('wineps5.so must not introduce an ELF TLS resolver import')
driver_poll = next((row for row in reviewed
                    if row['module'] == 'wineps5.so' and row['symbol'] == 'poll'), None)
if not driver_poll or (driver_poll['owner'], driver_poll['target']) != ('fself_public_or_platform', 'poll'):
    raise SystemExit('cursor negotiation is missing its reviewed poll import')

for symbol in ('malloc', 'free', 'memcpy', 'memset', 'pthread_mutex_lock',
               'pthread_mutex_unlock', 'pthread_once', '__wine_ps5_get_vulkan_proc'):
    row = next((row for row in reviewed
                if row['module'] == 'win32u.so' and row['symbol'] == symbol), None)
    owner = 'title_ntdll' if symbol == '__wine_ps5_get_vulkan_proc' else 'fself_public_or_platform'
    expected = (owner, symbol)
    if not row or (row['owner'], row['target']) != expected:
        raise SystemExit(f'title-GPU cursor dependency is not reviewed for win32u.so: {symbol}')
if any(row['module'] == 'win32u.so' and row['symbol'] == '__tls_get_addr' for row in reviewed):
    raise SystemExit('title-GPU cursor state must not introduce an ELF TLS resolver import')


def rejected(symbols, rows, description):
    try:
        make_records(symbols, rows)
    except ValueError:
        return
    raise SystemExit(f'import policy unexpectedly accepted {description}')


added = copy.deepcopy(module_symbols)
added['ntdll.so']['strong'].add('future_secret_runtime_api')
rejected(added, reviewed, 'an added symbol')

removed = copy.deepcopy(module_symbols)
removed_row = policy[0]
removed[removed_row['module']][removed_row['binding']].remove(removed_row['symbol'])
rejected(removed, reviewed, 'a removed symbol')

binding = copy.deepcopy(module_symbols)
binding_row = policy[1]
old_binding = binding_row['binding']
new_binding = 'weak' if old_binding == 'strong' else 'strong'
binding[binding_row['module']][old_binding].remove(binding_row['symbol'])
binding[binding_row['module']][new_binding].add(binding_row['symbol'])
rejected(binding, reviewed, 'binding drift')

with tempfile.TemporaryDirectory(prefix='pw-title-imports-') as directory:
    root = Path(directory)
    drift_policy = root / 'drift-policy.json'
    reviewed_document = json.loads(policy_path.read_text())
    policy_document = copy.deepcopy(reviewed_document)
    policy_document['imports'][0]['target'] = 'future_secret_runtime_api'
    drift_policy.write_text(json.dumps(policy_document))
    try:
        load_policy(drift_policy)
    except ValueError:
        pass
    else:
        raise SystemExit('import policy unexpectedly accepted target drift')
    policy_document = copy.deepcopy(reviewed_document)
    policy_document['native_roles']['child'] = copy.deepcopy(policy_document['native_roles']['parent'])
    drift_policy.write_text(json.dumps(policy_document))
    try:
        load_policy(drift_policy)
    except ValueError:
        pass
    else:
        raise SystemExit('import policy unexpectedly admitted child supervisor APIs')

    # This small link proof deliberately supplies a real definition for the
    # unknown name. The policy gate still rejects it before native link, and
    # the reviewed table consequently cannot resolve it.
    small_symbols = {module: {'strong': set(), 'weak': set()} for module in module_names()}
    small_symbols['ntdll.so']['strong'].add('dlopen')
    small_policy = [{
        'module': 'ntdll.so',
        'binding': 'strong',
        'symbol': 'dlopen',
        'target': 'pw_public_dlopen',
        'owner': 'custom_loader',
    }]
    small_records = make_records(small_symbols, small_policy)
    supplied_unknown = copy.deepcopy(small_symbols)
    supplied_unknown['ntdll.so']['strong'].add('future_secret_runtime_api')
    rejected(supplied_unknown, small_policy, 'an SDK/native-provided unknown symbol')

    paths = module_paths()
    modules = [{'name': name, 'path': paths[name], 'sha256': f'{index + 1:02x}' * 32,
                'size': index + 1} for index, name in enumerate(module_names())]
    win32u = module_names().index('win32u.so')
    try:
        render_include(small_records, modules[:win32u] + modules[win32u + 1:])
    except ValueError:
        pass
    else:
        raise SystemExit('missing win32u identity accepted')
    bad_path = copy.deepcopy(modules)
    bad_path[win32u]['path'] = '/app0/unreviewed/win32u.so'
    try:
        render_include(small_records, bad_path)
    except ValueError:
        pass
    else:
        raise SystemExit('tampered win32u path accepted')
    include = root / 'imports.inc'
    include.write_text(render_include(small_records, modules))
    identity_checks = ' ||\n'.join(
        f'           strcmp(title_modules[{index}].path, {json.dumps(item["path"])}) ||\n'
        f'           title_modules[{index}].size != UINT64_C({item["size"]}) ||\n'
        f'           title_modules[{index}].sha256[0] != 0x{item["sha256"][:2]}'
        for index, item in enumerate(modules))
    source = root / 'provided-unknown.c'
    source.write_text(f'''#include <stdint.h>
#include <string.h>
struct pw_elf_import {{ const char *name; uintptr_t address; }};
struct pw_title_module_manifest {{
    const char *path;
    uint8_t sha256[32];
    uint64_t size;
}};
#include "{include}"
#define DEFINE_EXACT(name, symbol) void name(void) __asm__(symbol); void name(void) {{}}
DEFINE_EXACT(exact_pw_public_dlopen, "pw_public_dlopen")
DEFINE_EXACT(exact_future_secret_runtime_api, "future_secret_runtime_api")
static uintptr_t lookup(const char *name) {{
    for (unsigned i = 0; i < sizeof(native_imports) / sizeof(native_imports[0]); ++i)
        if (!strcmp(name, native_imports[i].name)) return native_imports[i].address;
    return 0;
}}
int main(void) {{
    (void)&exact_future_secret_runtime_api;
    return lookup("dlopen") != (uintptr_t)&exact_pw_public_dlopen ||
           lookup("future_secret_runtime_api") != 0 ||
           sizeof(title_modules) / sizeof(title_modules[0]) != {len(modules)} ||
{identity_checks};
}}
''')
    executable = root / 'provided-unknown'
    command = shlex.split(args.cc) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)

print(f'TITLE_IMPORT_SELECTION_PASS imports={len(records)} '
      'unknown_import=POLICY_REJECTED_WITH_DEFINITION')
