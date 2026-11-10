#!/usr/bin/env python3
"""Host-only path, manifest and guarded-write tests for the B1 title profile."""
import hashlib
import importlib.util
import shutil
import json
from pathlib import Path
import tempfile

from runtime_profiles import (
    ADDON_WORKER_MAX_BYTES, ADDON_WORKER_RELATIVE, WRITEBROKER_MAX_BYTES,
    WRITEBROKER_RELATIVE, profile, installed_paths, runtime_tree_manifest,
    b1_guard_paths, qualified_b1_folder, file_identity, builtin_prefix_files, PRODUCT_KEYS,
    DEFAULT_TITLE_ID, title_identity, title_parameters,
)
from runtime_imports import (
    candidate_ownership, module_manifest, render_include, load_policy, make_records,
    native_role_imports,
)

with tempfile.TemporaryDirectory() as directory:
    tree = Path(directory)
    builtin = b'MZ' + bytes(62) + b'Wine builtin DLL\0' + bytes(16)
    for arch in ('x86_64', 'i386'):
        folder = tree / f'lib/wine/{arch}-windows'
        folder.mkdir(parents=True)
        for name in ('advapi32.dll', 'kernel32.dll', 'wineps5audio.drv', 'start.exe', 'dxgi.dll'):
            (folder / name).write_bytes(builtin)
        (folder / 'not-pe.txt').write_bytes(b'not a builtin PE')
        (folder / 'amd_ags_x64.dll').write_bytes(builtin)
    owned = dict(builtin_prefix_files(tree))
    for arch, folder in (('x86_64', 'system32'), ('i386', 'syswow64')):
        for name in ('advapi32.dll', 'kernel32.dll', 'wineps5audio.drv', 'start.exe'):
            assert owned[f'lib/wine/{arch}-windows/{name}'] == f'drive_c/windows/{folder}/{name}'
    assert not any('not-pe.txt' in name or 'amd_ags_x64.dll' in name for name in owned)
    assert list(owned) == sorted(owned)

b1 = profile('b1')
product = json.loads(Path(__file__).with_name('b1-product.json').read_text())
assert product['debug'] == 'err+all,err-seh'
assert product['argv'] == [r'C:\Forever\Addons\forever-maintenance.exe']
game_config = json.loads(Path(__file__).with_name('b1-game-wow.json').read_text())
maintenance = profile('b1', product)
assert maintenance['refused_programs'] == []
assert 'B1_GAME_PROFILE_CONFIG ?= $(CURDIR)/b1-game-wow.json' in (
    Path(__file__).with_name('Makefile').read_text())
game = profile('b1', game_config)
assert 'argv' not in game_config and 'cwd' not in game_config
assert game['argv'] == [game['loader']]
assert game['cwd'] is None and game['runtime_game'] is True
assert game['prefix'] == '/data/forever/prefix'
assert game['home'] == '/data/forever/home'
assert game['gpu'] is True and game['headless'] is True
assert game['refused_programs'] == ['BlizzardError.exe']
assert game['environment'] == {'PROTON_ENABLE_HDR': '1'}
assert 'WINEPS5_SDR_WHITE_NITS' not in game['environment']
assert set(b1['module_paths']) == {
    'ntdll.so', 'win32u.so', 'wineserver.so', 'ws2_32.so', 'crypt32.so', 'opengl32.so',
    'secur32.so', 'bcrypt.so', 'libgnutls.so', 'wineps5.so', 'dwrite.so', 'libfreetype.so',
}
assert b1['headless'] is False
assert b1['gpu'] is False
assert profile('b1', {'gpu': False})['gpu'] is False
assert profile('b1', {'gpu': True, 'headless': True})['gpu'] is True
gpu = profile('b1', {'headless': True, 'gpu': True})
assert gpu['gpu'] is True
assert gpu['module_paths'] == {**b1['module_paths'],
                               'wineps5audio.so': '/app0/wine-title/lib/wine/x86_64-unix/wineps5audio.so',
                               'winevulkan.so': '/app0/wine-title/lib/wine/x86_64-unix/winevulkan.so',
                               **{f'xinput1_{v}.so': f'/app0/wine-title/lib/wine/x86_64-unix/xinput1_{v}.so'
                                  for v in (1, 2, 3, 4)}}
assert 'winevulkan.so' not in profile('b1', {'headless': True})['module_paths']
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'PROTON_ENABLE_HDR': '1'}})['environment'] == {'PROTON_ENABLE_HDR': '1'}
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'WINEPS5_HDR_TRACE': '1'}})['environment'] == {'WINEPS5_HDR_TRACE': '1'}
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'WINEPS5_QUERY_TRACE': '1'}})['environment'] == {
                          'WINEPS5_QUERY_TRACE': '1'}
for direct in ('0', '1'):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {'WINEPS5_DIRECT_REQUESTS': direct}})['environment'] == {
                              'WINEPS5_DIRECT_REQUESTS': direct}
for log_async in ('0', '1'):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {'WINEPS5_LOG_ASYNC': log_async}})['environment'] == {
                              'WINEPS5_LOG_ASYNC': log_async}
for trace_name in ('WINEPS5_MXCSR_TRACE', 'WINEPS5_WRITE_TRACE', 'WINEPS5_WRITE_STICKY',
                   'WINEPS5_PROFILE'):
    for trace_value in ('0', '1'):
        for gpu_enabled in (False, True):
            assert profile('b1', {'gpu': gpu_enabled, 'headless': gpu_enabled,
                                  'environment': {trace_name: trace_value}})['environment'] == {
                                      trace_name: trace_value}
# Profiling is not a graphics feature: its startup assignment must run before
# the GPU-only environment branch as well as being admitted by profile().
native_startup = Path(__file__).with_name('native-entry.c').read_text()
common_environment = native_startup.split('if (native_runtime.payload_spawn', 1)[1].split(
    '#if PW_RUNTIME_GPU', 1)[0]
assert 'setenv("WINEPS5_PROFILE", PW_RUNTIME_PROFILE, 0)' in common_environment
# Source trace of the shared pre-Wine gate: valid Play, direct-game and game
# restarts all enter this main. Managed-copy failure behavior is exercised by
# test-forever-setup; these assertions keep both role entry paths behind it.
startup_main = native_startup.split('int main(int argc, char **argv)', 1)[1]
setup_gate = startup_main.index('if (pw_forever_start())')
direct_handoff = startup_main.index('pw_maintenance_direct_game()')
game_wine_entry = startup_main.index('wine_public_title_main(parent_argc,args)')
assert setup_gate < direct_handoff < game_wine_entry
assert 'return 1;' in startup_main[setup_gate:direct_handoff]
sync_stage = native_startup.split('static int pw_forever_start(void)', 1)[1].split(
    'result = forever_install_wow_config', 1)[0]
assert 'result = pw_forever_sync_managed();' in sync_stage
assert 'if (result < 0)' in sync_stage.split('result = pw_forever_sync_managed();', 1)[1]
assert 'return -1;' in sync_stage.split('io_log("forever: app-owned prefix files', 1)[1]
assert 'forever: prefix sync checked=%u hashed=%u repaired=%u' in native_startup
# The menu's display settings reach games only: the GPU role reads
# C:\wineps5-display.txt once at start and logs what it set.
gpu_environment = native_startup.split('if (native_runtime.payload_spawn', 1)[1].split(
    '#if PW_RUNTIME_GPU', 1)[1].split('#else', 1)[0]
assert 'forever_display_read(FOREVER_DRIVE_C "/" FOREVER_DISPLAY_FILE, &display)' in gpu_environment
assert 'if (forever_display_environment(&display)) return 1;' in gpu_environment
assert 'VKD3D_FRAME_RATE=%s DXVK_CONFIG=%s' in gpu_environment
assert 'forever_display_environment' not in common_environment
# Only intercepted Play commands publish launch state. Replaying/falling back
# must not turn a synthesized direct command into a purported Play record.
assert 'pw_game_record_launch(FOREVER_STATE, (char **)game_wire.argv + 7,' in native_startup
assert 'pw_game_direct_launch(&replay, FOREVER_STATE, &channel, &recorded)' in native_startup
assert 'pw_parent_stage_game_launch((char **)replay.argv + 7, replay.argv[5], replay.argv[3], 0)' in native_startup
assert 'pw_parent_stage_game_launch(launch->argv, launch->windows_cwd, launch->unix_cwd, 1)' in native_startup
supervisor_source = Path(__file__).with_name('runtime-parent.c').read_text()
assert 'pw_parent_stage_game_launch(argv + 7, argv[5], argv[3], 1)' in supervisor_source
assert profile('b1', {'gpu': True, 'headless': True,
                      'environment': {'PROTON_ENABLE_HDR': '1', 'WINEPS5_DIRECT_REQUESTS': '0',
                                      'WINEPS5_HDR_TRACE': '1', 'WINEPS5_QUERY_TRACE': '1'}})['environment'] == {
                          'PROTON_ENABLE_HDR': '1', 'WINEPS5_DIRECT_REQUESTS': '0',
                          'WINEPS5_HDR_TRACE': '1', 'WINEPS5_QUERY_TRACE': '1'}
for name, setting in (
        ('WINEPS5_PROFILE', '1'),
        ('PS5_VIDEOOUT_STATS', '1'),
        ('VKD3D_CONFIG', 'single_queue'),
        ('VKD3D_CONFIG', 'single_queue,no_staggered_submit'),
        ('VKD3D_QUEUE_PROFILE', r'W:\vkd3d-queue-profile.json'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'FIFO'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'IMMEDIATE'),
        ('VKD3D_SWAPCHAIN_PRESENT_MODE', 'MAILBOX'),
        ('WINEPS5_SDR_WHITE_NITS', '80'),
        ('WINEPS5_SDR_WHITE_NITS', '203'),
        ('WINEPS5_SDR_WHITE_NITS', '480'),
        ('WINEPS5_HDR_MODE', 'mode=off'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=80'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=637'),
        ('WINEPS5_HDR_MODE', 'mode=cap max=10000'),
        ('WINEPS5_HDR_MODE', 'mode=sdr'),
        ('WINEPS5_HDR_MODE', 'mode=sdr paper=203 peak=637 expand=1 sat=1.0 gamma=2.2 key=y'),
        ('WINEPS5_HDR_MODE', 'mode=sdr gamma=srgb key=max'),
        ('WINEPS5_HDR_MODE', 'mode=auto sdr_shaders=52596c4112cfc065 '
                             'hdr_shaders=c2d9013c5a1bd61e paper=203 peak=637 '
                             'expand=0.4 sat=1.0 gamma=2.6 key=y'),
        ('WINEPS5_HDR_MODE', 'mode=auto sdr_shaders=52596c4112cfc065,0123456789abcdef '
                             'hdr_shaders=c2d9013c5a1bd61e hdr_cap=637')):
    assert profile('b1', {'gpu': True, 'headless': True,
                          'environment': {name: setting}})['environment'] == {name: setting}
game_mode = profile('b1', {'gpu': True, 'headless': True,
                           'child_dll_overrides': 'vulkan-1=n,b;dxgi=b'})
assert game_mode['child_dll_overrides'] == 'vulkan-1=n,b;dxgi=b'
assert b1['child_dll_overrides'] == b1['dll_overrides']
for setting in ({'DXVK_HDR': '1'}, {'PATH': '/app0'}, {'PROTON_ENABLE_HDR': 1},
                {'PROTON_ENABLE_HDR': 'yes'}, {'WINEPS5_DIRECT_REQUESTS': 0},
                {'WINEPS5_DIRECT_REQUESTS': 'yes'}, {'WINEPS5_HDR_TRACE': 1},
                {'WINEPS5_HDR_TRACE': 'yes'}, {'WINEPS5_QUERY_TRACE': 1},
                {'WINEPS5_QUERY_TRACE': 'yes'}, {'PS5_VIDEOOUT_STATS': '0'},
                {'WINEPS5_PROFILE': 1}, {'WINEPS5_PROFILE': 'yes'},
                {'WINEPS5_MXCSR_TRACE': 1}, {'WINEPS5_MXCSR_TRACE': 'yes'},
                {'WINEPS5_WRITE_TRACE': 1}, {'WINEPS5_WRITE_TRACE': 'yes'},
                {'WINEPS5_WRITE_STICKY': 1}, {'WINEPS5_WRITE_STICKY': 'yes'},
                {'VKD3D_CONFIG': 'no_staggered_submit'},
                {'VKD3D_QUEUE_PROFILE': '/tmp/profile.json'},
                {'VKD3D_SWAPCHAIN_PRESENT_MODE': 'FIFO_RELAXED'},
                {'WINEPS5_HDR_MAX_NITS': '637'}, {'WINEPS5_HDR_MODE': 637},
                {'WINEPS5_SDR_WHITE_NITS': 203},
                {'WINEPS5_SDR_WHITE_NITS': '79'},
                {'WINEPS5_SDR_WHITE_NITS': '481'},
                {'WINEPS5_SDR_WHITE_NITS': '203.0'},
                {'WINEPS5_SDR_WHITE_NITS': '0203'},
                {'WINEPS5_HDR_MODE': 'sdr'}, {'WINEPS5_HDR_MODE': 'mode=cap'},
                {'WINEPS5_HDR_MODE': 'mode=cap max=79'},
                {'WINEPS5_HDR_MODE': 'mode=cap max=10001'},
                {'WINEPS5_HDR_MODE': 'mode=off peak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr paper=638 peak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr expand=1.1'},
                {'WINEPS5_HDR_MODE': 'mode=sdr sat=2.1'},
                {'WINEPS5_HDR_MODE': 'mode=sdr gamma=0.9'},
                {'WINEPS5_HDR_MODE': 'mode=sdr key=red'},
                {'WINEPS5_HDR_MODE': 'mode=sdr\npeak=637'},
                {'WINEPS5_HDR_MODE': 'mode=sdr sdr_shaders=52596c4112cfc065'},
                {'WINEPS5_HDR_MODE': 'mode=auto hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 hdr_shaders=short'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065,52596c4112cfc065 '
                                     'hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 '
                                     'hdr_shaders=52596c4112cfc065'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=0000000000000001,0000000000000002,'
                                     '0000000000000003,0000000000000004,0000000000000005 '
                                     'hdr_shaders=c2d9013c5a1bd61e'},
                {'WINEPS5_HDR_MODE': 'mode=auto sdr_shaders=52596c4112cfc065 '
                                     'hdr_shaders=c2d9013c5a1bd61e hdr_cap=79'},
                [], None):
    try:
        profile('b1', {'gpu': True, 'headless': True, 'environment': setting})
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 accepted unsafe environment: {setting}')
try:
    profile('b1', {'environment': {'PROTON_ENABLE_HDR': '1'}})
except ValueError:
    pass
else:
    raise AssertionError('product profile accepted a GPU environment')
assert b1['payload_programs'] == []
assert b1['refused_programs'] == []
assert profile('b1', {'refused_programs': ['BlizzardError.exe']})['refused_programs'] == [
    'BlizzardError.exe']
assert b1['installer_dos'] == r'C:\Battle.net-Setup.exe'
assert b1['installer_url'] == 'https://downloader.battle.net//download/getInstallerForGame?os=win&gameProgram=BATTLENET_APP&version=Live'
# A direct versioned download, tried only after the Live redirect fails.
assert b1['installer_fallback_url'] == 'https://downloader.battle.net/download/installer/win/1.0.66/Battle.net-Setup.exe'
assert b1['product_name'] == 'Battle.net' and b1['installer_args'] == []
assert b1['launcher_dos'] == r'C:\Program Files (x86)\Battle.net\Battle.net Launcher.exe'
# Paths derived from the prefix are never inputs.
for key in ('launcher_unix', 'installer_unix', 'installer_part', 'installer_cwd'):
    try:
        profile('b1', {key: b1[key]})
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 profile accepted derived installer field: {key}')
relocated = profile('b1', {'prefix': '/app0/wine/prefix'})
assert relocated['installer_unix'] == '/app0/wine/prefix/drive_c/Battle.net-Setup.exe'
assert relocated['installer_part'] == '/app0/wine/prefix/drive_c/.Battle.net-Setup.exe.part'
assert relocated['launcher_unix'] == '/app0/wine/prefix/drive_c/Program Files (x86)/Battle.net/Battle.net Launcher.exe'
assert relocated['installer_cwd'] == '/app0/wine/prefix/drive_c'
# Battle.net's values, restated as inputs, select the same profile. It has no
# installer digest or unpack member.
assert 'installer_sha256' not in b1 and 'installer_unpack' not in b1
assert profile('b1', {key: b1[key] for key in PRODUCT_KEYS if key in b1}) == b1
other = profile('b1', {
    'prefix': '/data/forever/other/prefix', 'product_name': 'Other',
    'launcher_dos': r'C:\Program Files (x86)\Other\other.exe',
    'installer_url': 'https://cdn.example.invalid/OtherSetup.exe',
    'installer_dos': r'C:\OtherSetup.exe', 'installer_args': ['/S']})
assert other['launcher_unix'] == '/data/forever/other/prefix/drive_c/Program Files (x86)/Other/other.exe'
assert other['installer_unix'] == '/data/forever/other/prefix/drive_c/OtherSetup.exe'
assert other['installer_part'] == '/data/forever/other/prefix/drive_c/.OtherSetup.exe.part'
assert other['installer_cwd'] == '/data/forever/other/prefix/drive_c'
assert other['installer_fallback_url'] == ''  # Battle.net's fallback never follows another URL
assert other['installer_args'] == ['/S'] and other['product_name'] == 'Other'
assert profile('b1', {'installer_url': 'https://a.example.invalid/x.exe',
                      'installer_fallback_url': 'https://b.example.invalid/x.exe'})[
    'installer_fallback_url'] == 'https://b.example.invalid/x.exe'
for key, bad in (
        ('product_name', ''), ('product_name', 'x' * 33), ('product_name', '100%'),
        ('product_name', 'a"b'), ('product_name', 'a\\b'), ('product_name', 'caf\u00e9'), ('product_name', 7),
        ('launcher_dos', 'Program Files\\x.exe'), ('launcher_dos', r'D:\x.exe'), ('launcher_dos', 'C:\\'),
        ('launcher_dos', r'C:\a\..\b.exe'), ('launcher_dos', r'C:\a\\b.exe'), ('launcher_dos', 'C:\\a/b.exe'),
        ('launcher_dos', 'C:\\a;b.exe'), ('launcher_dos', 'C:\\a.exe\0'), ('launcher_dos', r'C:\dir.\a.exe'),
        ('installer_dos', r'C:\Temp\Setup.exe'), ('installer_dos', r'C:\Setup.msi'), ('installer_dos', r'C:\..'),
        ('installer_url', 'http://cdn.example.invalid/x.exe'), ('installer_url', ''),
        ('installer_url', 'https://'), ('installer_url', 'https://user@cdn.example.invalid/x.exe'),
        ('installer_url', 'https://cdn.example.invalid/a b.exe'), ('installer_url', 'https://cdn.example.invalid/a"'),
        ('installer_url', 'https://cdn.example.invalid/\u00e9'), ('installer_url', 'https://' + 'a' * 8192),
        ('installer_fallback_url', 'ftp://cdn.example.invalid/x.exe'),
        ('installer_args', '/S'), ('installer_args', ['']), ('installer_args', ['/S'] * 9),
        ('installer_args', ['a"b']), ('installer_args', ['a\nb']), ('installer_args', [1])):
    try:
        profile('b1', {key: bad})
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 profile accepted {key}={bad!r}')
assert profile('b1', {'installer_fallback_url': ''})['installer_fallback_url'] == ''
# Unpack mode: a verified zip whose one file becomes launcher_dos; nothing runs.
digest = hashlib.sha256(b'zip').hexdigest()
unpack_inputs = {
    'product_name': 'Unpacked', 'launcher_dos': r'C:\Program Files (x86)\Unpacked\unpacked.exe',
    'installer_url': 'https://cdn.example.invalid/unpacked.zip', 'installer_dos': r'C:\unpacked.zip',
    'installer_sha256': digest, 'installer_unpack': 'unpacked.exe'}
unpacked = profile('b1', unpack_inputs)
assert unpacked['installer_unpack'] == 'unpacked.exe' and unpacked['installer_sha256'] == digest
assert unpacked['installer_unix'] == b1['prefix'] + '/drive_c/unpacked.zip'
assert unpacked['installer_part'] == b1['prefix'] + '/drive_c/.unpacked.zip.part'
assert unpacked['launcher_unix'] == b1['prefix'] + '/drive_c/Program Files (x86)/Unpacked/unpacked.exe'
assert unpacked['installer_args'] == [] and unpacked['installer_fallback_url'] == ''
# A digest alone also checks an .exe installer's download.
assert profile('b1', {'installer_sha256': digest})['installer_sha256'] == digest
for change in (
        {'installer_sha256': digest.upper()}, {'installer_sha256': digest[:63]},
        {'installer_sha256': 'g' * 64}, {'installer_sha256': 7},
        {'installer_unpack': ''}, {'installer_unpack': '..'}, {'installer_unpack': '.'},
        {'installer_unpack': '../unpacked.exe'}, {'installer_unpack': 'dir/unpacked.exe'},
        {'installer_unpack': 'dir\\unpacked.exe'}, {'installer_unpack': 'C:unpacked.exe'},
        {'installer_unpack': '/unpacked.exe'}, {'installer_unpack': 'unpacked.exe.'},
        {'installer_unpack': 'a\nb.exe'}, {'installer_unpack': 'x' * 256},
        {'installer_unpack': 'caf\u00e9.exe'}, {'installer_unpack': 7},
        {'installer_sha256': None},  # removed below: unpack requires a digest
        {'installer_args': ['/S']},  # nothing runs, so nothing takes arguments
        {'installer_dos': r'C:\unpacked.exe'},  # the download is a zip
        {'installer_dos': r'C:\Temp\unpacked.zip'}):
    inputs = {**unpack_inputs, **change}
    if inputs['installer_sha256'] is None:
        del inputs['installer_sha256']
    try:
        profile('b1', inputs)
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 unpack profile accepted {change!r}')
try:
    profile('b1', {'installer_dos': r'C:\Setup.zip'})
except ValueError:
    pass
else:
    raise AssertionError('B1 profile accepted a .zip installer it would run')
# A side-by-side test app changes only the app's identity, from the environment.
paths_sh = (Path(__file__).resolve().parents[2] / 'paths.sh').read_text()
assert f'TITLE_ID={DEFAULT_TITLE_ID}\n' in paths_sh
source_param = json.loads((Path(__file__).resolve().parents[2] / 'title/sce_sys/param.json').read_text())
assert title_identity({}) == (DEFAULT_TITLE_ID, None)
assert title_parameters(source_param, {}) == source_param
test_app = title_parameters(source_param, {'FOREVER_TITLE_ID': 'PPSA99031',
                                           'FOREVER_TITLE_NAME': 'Forever Steam Test'})
assert test_app == {**source_param, 'titleId': 'PPSA99031', 'conceptId': '99031',
                    'contentId': source_param['contentId'].replace(DEFAULT_TITLE_ID, 'PPSA99031'),
                    'localizedParameters': {**source_param['localizedParameters'],
                                            'en-US': {'titleName': 'Forever Steam Test'}}}
assert 'PPSA99030' not in json.dumps(test_app)
for environ in ({'FOREVER_TITLE_ID': 'ppsa99031'}, {'FOREVER_TITLE_ID': 'PPSA9903'},
                {'FOREVER_TITLE_ID': 'PPSA99031/x'}, {'FOREVER_TITLE_NAME': 'a"b'},
                {'FOREVER_TITLE_NAME': 'x' * 65}, {'FOREVER_TITLE_NAME': 'caf\u00e9'}):
    try:
        title_identity(environ)
    except ValueError:
        pass
    else:
        raise AssertionError(f'title identity accepted {environ}')
build_source = Path(__file__).with_name('build-runtime.py').read_text()
assert "'#define PW_RUNTIME_TITLE_ID ' + json.dumps(title_identity()[0])" in build_source
assert 'metadata = title_parameters(built_metadata)' in build_source
assert 'expected_parameter = title_parameters(base_parameter)' in (
    Path(__file__).with_name('inspect-runtime.py').read_text())
# The broker puts the icon on WB_TITLE_ID's entry: another app must not ask.
icon_start = native_startup.split('static void pw_title_icon_start(void)', 1)[1].split('\n}', 1)[0]
assert icon_start.index('if (strcmp(PW_RUNTIME_TITLE_ID, WB_TITLE_ID)) return;') < icon_start.index('create_thread')
# The title's installer path reads the product only from the profile.
download = native_startup.split('static void b1_download_progress', 1)[1].split('#ifdef PW_PARENT_ROLE', 1)[0]
assert 'Battle.net' not in download and 'PW_RUNTIME_PRODUCT_NAME' in download
assert 'const char *part = PW_RUNTIME_INSTALLER_PART;' in download
assert 'sizeof(urls) / sizeof(urls[0]) && urls[i][0]' in download
assert 'char *installer_args[]=PW_RUNTIME_INSTALLER_ARGV;' in native_startup
assert 'installer ? 4 :' not in native_startup
assert native_startup.count('PW_RUNTIME_INSTALLER_ARGC') == 2
assert "'/exec',\n                  selected['installer_dos'], *selected['installer_args']]" in build_source
# Unpack mode is compiled only into a product that sets it; Battle.net's
# build gets neither its defines nor miniz.
assert "for key in ('installer_sha256', 'installer_unpack') if key in selected)" in build_source
assert "'installer_unpack' in selected and not selected.get('gpu', False)" in build_source
assert '*unpack_sources, *io_sources' in build_source
assert '.cap = B1_INSTALLER_CAP, .mz = B1_INSTALLER_MZ, .sha256 = B1_INSTALLER_SHA256,' in download
unpack_start = native_startup.split('installer = !forever_prefix_init && stat(PW_RUNTIME_LAUNCHER_UNIX', 1)[1]
unpack_start = unpack_start.split('title_desktop_installer_mode', 1)[0]
assert unpack_start.index('#ifdef PW_RUNTIME_INSTALLER_UNPACK') < unpack_start.index(
    'if (installer && b1_unpack_installer())') < unpack_start.index('installer = 0;') < unpack_start.index('#else')
unpack_step = native_startup.split('static int b1_unpack_installer(void)', 1)[1].split('\n}', 1)[0]
assert 'Battle.net' not in unpack_step and 'unlink(PW_RUNTIME_INSTALLER_UNIX);' in unpack_step
assert unpack_step.index('b1_download_installer()') < unpack_step.index('forever_unpack_zip(')
assert profile('b1', {'payload_programs': [r'C:\ProgramData\Battle.net\Agent\Agent.exe']})['payload_programs'] == [
    r'C:\ProgramData\Battle.net\Agent\Agent.exe']
assert ('"WINEDLLOVERRIDES=' + b1['payload_dll_overrides'] + '"') in (
    Path(__file__).resolve().parents[2] / 'deploy-wine.py').read_text()
assert profile('b1', {'payload_programs': [r'D:\Tools\Switcher.exe']})['payload_programs'] == [r'D:\Tools\Switcher.exe']
for headless in (False, True):
    roles = native_role_imports(headless=headless)
    assert all(roles['parent'][name] == 'libkernel.prx'
               for name in ('connect', 'setsockopt', 'shutdown'))
    assert roles['parent']['sceSystemServiceLaunchWebBrowser'] == 'libSceSystemService.prx'
    assert roles['parent']['sceUserServiceInitialize'] == 'libSceUserService.prx'
    assert roles['child'] == {}
gpu_roles = native_role_imports(headless=True, gpu=True)
assert gpu_roles['child'] == native_role_imports(headless=True)['child']
assert gpu_roles['parent']['sceAgcInit'] == 'libSceAgc.prx'
assert gpu_roles['parent']['sceAgcDriverSubmitDcb'] == 'libSceAgcDriver.prx'
assert gpu_roles['parent']['sceVideoOutOpen'] == 'libSceVideoOut.prx'
for symbol in ('sceAudioOutInit', 'sceAudioOutOpen', 'sceAudioOutOutput', 'sceAudioOutClose'):
    assert gpu_roles['parent'][symbol] == 'libSceAudioOut.prx'
    assert symbol not in gpu_roles['child']
    assert symbol not in native_role_imports(headless=False)['parent']
    assert symbol not in native_role_imports(headless=True)['parent']
for symbol in ('sceVideoOutCursorDisable', 'sceVideoOutCursorEnable',
               'sceVideoOutCursorSetHotSpot', 'sceVideoOutCursorSetPosition'):
    assert gpu_roles['parent'][symbol] == 'libSceVideoOut.prx'
    assert symbol not in gpu_roles['child']
    assert symbol not in native_role_imports(headless=False)['parent']
    assert symbol not in native_role_imports(headless=True)['parent']
for symbol, provider in (
        ('scePadRead', 'libScePad.prx'),
        ('sceKeyboardClose', 'libSceKeyboard.prx'),
        ('sceKeyboardRead', 'libSceKeyboard.prx'),
        ('sceKeyboardReadState', 'libSceKeyboard.prx'),
        ('sceKeyboardSetProcessPrivilege', 'libSceKeyboard.prx'),
        ('sceMouseRead', 'libSceMouse.prx'),
        ('sceUserServiceGetLoginUserIdList', 'libSceUserService.prx'),
        ('sceImeOpen', 'libSceIme.prx')):
    assert gpu_roles['parent'][symbol] == provider
    assert native_role_imports(headless=False)['parent'][symbol] == provider
    assert symbol not in native_role_imports(headless=True)['parent']
    assert symbol not in gpu_roles['child']
assert gpu_roles['parent']['scePadSetVibration'] == 'libScePad.prx'
assert 'scePadSetVibration' not in gpu_roles['child']
for roles in (native_role_imports(), native_role_imports(headless=True)):
    assert 'scePadSetVibration' not in roles['parent']
assert not any(name.startswith('xinput') for name in b1['module_paths'])
assert 'sceHttp2Init' not in gpu_roles['parent']
for roles in (native_role_imports(), native_role_imports(headless=True)):
    assert 'sceAgcInit' not in roles['parent']
    assert 'sceAgcDriverSubmitDcb' not in roles['parent']
voice_symbols = {'sceAudioInInit', 'sceAudioInOpen', 'sceAudioInInput', 'sceAudioInClose'}
assert {name for name in gpu_roles['parent'] if name.startswith('sceAudioIn')} == voice_symbols
assert all(gpu_roles['parent'][name] == 'libSceAudioIn.prx' for name in voice_symbols)
assert gpu_roles['parent']['scePthreadSetprio'] == 'libkernel.prx'
for roles in (native_role_imports(), native_role_imports(headless=True)):
    assert not voice_symbols.intersection(roles['parent'])
    assert not voice_symbols.intersection(roles['child'])
assert not voice_symbols.intersection(gpu_roles['child'])
for symbol in ('sceHttp2AbortRequest', 'sceHttp2AddRequestHeader', 'sceHttp2GetAllResponseHeaders',
               'sceHttp2SetAutoRedirect', 'sceHttp2SetResolveTimeOut',
               'sceHttp2SetConnectTimeOut', 'sceHttp2SetRecvTimeOut'):
    assert native_role_imports()['parent'][symbol] == 'libSceHttp2.prx'
    assert symbol not in gpu_roles['parent'] and symbol not in gpu_roles['child']
reviewed = load_policy(Path(__file__).with_name('runtime-import-policy-b1.json'))
for symbol in ('NtUserGetForegroundWindow', 'NtUserGetGUIThreadInfo', 'NtUserPostMessage', 'NtUserQueryWindow'):
    assert any(row == {'module': 'wineps5.so', 'binding': 'strong', 'owner': 'title_win32u',
                       'symbol': symbol, 'target': symbol} for row in reviewed)
gpu_surfaces = {name: {'strong': set(), 'weak': set()} for name in gpu['module_paths']}
for row in reviewed:
    gpu_surfaces[row['module']][row['binding']].add(row['symbol'])
assert any('winevulkan.so' in row['modules'] for row in make_records(gpu_surfaces, reviewed, gpu=True))
assert any('wineps5audio.so' in row['modules']
           for row in make_records(gpu_surfaces, reviewed, gpu=True))
assert any(row['name'] == 'ps5_title_profile_set_blocking' and row['owner'] == 'title_ntdll'
           and set(row['modules']) == {'wineps5audio.so', 'winevulkan.so'}
           for row in make_records(gpu_surfaces, reviewed, gpu=True))
assert gpu_roles['parent']['pthread_getcpuclockid'] == 'libkernel.prx'
try:
    make_records(gpu_surfaces, reviewed)
except ValueError:
    pass
else:
    raise AssertionError('a launcher build accepted the GPU module')
surfaces = {name: gpu_surfaces[name] for name in b1['module_paths']}
authorized = make_records(surfaces, reviewed)
assert not any('winevulkan.so' in row['modules'] for row in authorized)
assert not any('wineps5audio.so' in row['modules'] for row in authorized)
assert any(row['owner'] == 'title_win32u' for row in authorized)
assert any(row['name'] == 'getaddrinfo' and row['target'] == 'pw_b1_getaddrinfo' for row in authorized)
# B1 klog writes leave on the native writer thread (title-log.c).
assert candidate_ownership('sceKernelDebugOutText') == ('platform', 'pw_title_klog_text')
assert {row['module'] for row in reviewed if row['symbol'] == 'sceKernelDebugOutText'} == {
    'ntdll.so', 'win32u.so', 'wineps5.so', 'wineserver.so'}
assert all(row['target'] == 'pw_title_klog_text' for row in authorized if row['name'] == 'sceKernelDebugOutText')
surfaces['ws2_32.so']['strong'].add('future_unreviewed_network_function')
try:
    make_records(surfaces, reviewed)
except ValueError:
    pass
else:
    raise AssertionError('B1 import policy accepted a new resolver import')
assert b1['root'] == '/app0/wine-title'
assert installed_paths(b1['loader']) == {
    'dll_dir': '/app0/wine-title/lib/wine',
    'bin_dir': '/app0/wine-title/bin',
    'data_dir': '/app0/wine-title/share/wine',
    'wineloader': b1['loader'],
}
assert b1['prefix'] == '/app0/wine/tests/b1-trace/prefix'
assert b1['home'] == '/app0/wine/tests/b1-trace/home'
assert b1['server'] == '/app0/wine-title/server-title'
custom = profile('b1', {'argv': ['start.exe', '/exec', 'custom.exe', '--disable-gpu'],
                        'cwd': '/app0/custom', 'prefix': '/app0/wine/custom-prefix',
                        'home': '/app0/wine/custom-home', 'dll_overrides': 'wineps5.drv=d',
                        'headless': True, 'payload_programs': []})
assert custom['argv'] == [b1['loader'], 'start.exe', '/exec', 'custom.exe', '--disable-gpu']
assert custom['module_paths'] == b1['module_paths'] and custom['root'] == b1['root']
assert custom['headless'] is True
assert custom['payload_programs'] == []
assert custom['payload_dll_overrides'] == b1['payload_dll_overrides']
for invalid in ({'root': '/app0/wine'}, {'prefix': '/app0/../data'}, {'argv': []},
                {'headless': 'yes'}, {'gpu': 'yes'}, {'gpu': 1}, {'gpu': True},
                {'gpu': True, 'headless': False}, {'payload_programs': 'Agent.exe'},
                {'payload_programs': ['Agent.exe']}, {'payload_programs': [r'C:\Agent.exe;C:\Other.exe']},
                {'payload_programs': ['C:\\Agent.exe\n']}, {'payload_programs': [None]},
                {'refused_programs': 'BlizzardError.exe'},
                {'refused_programs': [r'C:\BlizzardError.exe']},
                {'refused_programs': ['Utils/BlizzardError.exe']},
                {'refused_programs': ['BlizzardError.exe;Other.exe']},
                {'refused_programs': ['BlizzardError.exe', 'blizzarderror.EXE']},
                {'refused_programs': ['BlizzárdError.exe']}, {'refused_programs': [None]},
                {'gpu': True, 'headless': True, 'child_dll_overrides': ''},
                {'gpu': True, 'headless': True, 'child_dll_overrides': 'dxgi=b\nsecret'},
                {'child_dll_overrides': 'dxgi=b'}):
    try:
        profile('b1', invalid)
    except ValueError:
        pass
    else:
        raise AssertionError(f'B1 profile accepted unsafe input: {invalid}')

with tempfile.TemporaryDirectory(prefix='pw-b1-profile-') as temporary:
    root = Path(temporary) / 'tree'
    for relative in (WRITEBROKER_RELATIVE, ADDON_WORKER_RELATIVE,
                     'lib/wine/i386-windows/ntdll.dll',
                     'lib/wine/x86_64-windows/ntdll.dll', 'share/wine/nls/locale.nls',
                     'share/wine/wine.inf', 'lib/wine/x86_64-unix/wine',
                     'lib/wine/x86_64-unix/ntdll.so', 'lib/wine/x86_64-unix/win32u.so',
                     'bin/wineserver.so', 'lib/wine/x86_64-unix/ws2_32.so',
                     'lib/wine/x86_64-unix/crypt32.so', 'lib/wine/x86_64-unix/opengl32.so',
                     'lib/wine/x86_64-unix/secur32.so', 'lib/wine/x86_64-unix/bcrypt.so',
                     'lib/wine/x86_64-unix/wineps5.so', 'lib/libgnutls.so',
                     'lib/wine/x86_64-unix/dwrite.so', 'lib/libfreetype.so'):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(
            b'\x7fELFhost-test'
            if relative in (WRITEBROKER_RELATIVE, ADDON_WORKER_RELATIVE)
            else relative.encode())
    manifest = runtime_tree_manifest(root, b1)
    assert manifest['remote_root'] == '/data/homebrew/PPSA99030/wine-title'
    assert manifest['pe_dll_counts'] == {'i386': 1, 'x86_64': 1}
    assert manifest['files']['share/wine/nls/locale.nls']['sha256'] == hashlib.sha256(
        b'share/wine/nls/locale.nls').hexdigest()
    assert manifest['files'][WRITEBROKER_RELATIVE] == file_identity(root / WRITEBROKER_RELATIVE)
    assert (root / WRITEBROKER_RELATIVE).stat().st_size <= WRITEBROKER_MAX_BYTES
    assert manifest['files'][ADDON_WORKER_RELATIVE] == file_identity(root / ADDON_WORKER_RELATIVE)
    assert (root / ADDON_WORKER_RELATIVE).stat().st_size <= ADDON_WORKER_MAX_BYTES
    modules = module_manifest({name: root / relative.removeprefix('/app0/wine-title/')
                              for name, relative in b1['module_paths'].items()})
    assert all(item['path'].startswith('/app0/wine-title/') for item in modules)
    assert '/app0/wine-title/bin/wineserver.so' in render_include([], modules)
    folder = Path(temporary) / 'published'
    for relative in ('eboot.bin', 'sce_sys/param.json', 'sce_module/libc.prx',
                     'assets/icon0.png', 'psmkit-wine-runtime-child.bin',
                     'runtime-qualification.json'):
        path = folder / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'published')
    guards = b1_guard_paths(folder, '/data/homebrew/PPSA99030')
    assert '/data/homebrew/PPSA99030/wine/boot.txt' in guards
    assert '/data/homebrew/PPSA99030/assets/icon0.png' in guards
    assert '/data/homebrew/PPSA99030/.upload-eboot.bin' in guards
    assert '/data/homebrew/PPSA99030/sce_sys/.upload-param.json' in guards
    assert not any('/wine/lib/wine/' in path or '/wine-title/' in path for path in guards)
    (root / 'title-root-manifest.json').write_text(json.dumps(manifest, sort_keys=True))
    shutil.copytree(root, folder / 'wine-title')
    def inventory(directory):
        return [{'path': path.relative_to(directory).as_posix(),
                 'size': path.stat().st_size, 'sha256': file_identity(path)['sha256']}
                for path in sorted(directory.rglob('*')) if path.is_file()]
    broker_identity = {
        'path': WRITEBROKER_RELATIVE,
        'app_path': b1['root'] + '/' + WRITEBROKER_RELATIVE,
        'classification': 'data_only_not_loaded',
        'size': (root / WRITEBROKER_RELATIVE).stat().st_size,
        'sha256': file_identity(root / WRITEBROKER_RELATIVE)['sha256'],
    }
    addon_worker_identity = {
        'path': ADDON_WORKER_RELATIVE,
        'app_path': b1['root'] + '/' + ADDON_WORKER_RELATIVE,
        'classification': 'data_only_not_loaded',
        'size': (root / ADDON_WORKER_RELATIVE).stat().st_size,
        'sha256': file_identity(root / ADDON_WORKER_RELATIVE)['sha256'],
    }
    material = {
        'profile': b1, 'modules': modules, 'writebroker': broker_identity,
        'addon_worker': addon_worker_identity,
        'title_root': inventory(folder / 'wine-title'),
        'parent_title': [item for item in inventory(folder)
                         if not item['path'].startswith('wine-title/') and item['path'] not in
                         ('runtime-qualification.json', 'psmkit-wine-runtime-child.bin')],
        'native_parent': {'fself_size': 9, 'fself_sha256': hashlib.sha256(b'published').hexdigest()},
        'native_child': {'fself_size': 9, 'fself_sha256': hashlib.sha256(b'published').hexdigest()},
    }
    receipt = {
        'status': 'QUALIFIED_STATIC_NOT_EXECUTED', 'material': material,
        'qualification_key': hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
    }
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    assert qualified_b1_folder(folder) == (receipt, manifest)
    # The same channel-neutral role must survive both its own qualification
    # and the combined maintenance/game receipt reconstruction.
    added_modules = []
    for name, installed in game['module_paths'].items():
        destination = folder / installed.removeprefix('/app0/')
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(name.encode())
            added_modules.append(destination)
    game_manifest = runtime_tree_manifest(folder / 'wine-title', game)
    (folder / 'wine-title/title-root-manifest.json').write_text(json.dumps(game_manifest))
    original_material = dict(material)
    for dual in (False, True):
        material['profile'] = maintenance if dual else game
        if dual:
            material['game_profile'] = game
        material['title_root'] = inventory(folder / 'wine-title')
        receipt['qualification_key'] = hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
        assert qualified_b1_folder(folder) == (receipt, game_manifest)
    for destination in added_modules:
        destination.unlink()
    material.clear()
    material.update(original_material)
    (folder / 'wine-title/title-root-manifest.json').write_text(json.dumps(manifest, sort_keys=True))
    for selected_profile, accepted in ((profile('b1', {'gpu': True, 'headless': True}), False),
                                       ({key: value for key, value in b1.items() if key != 'gpu'}, True),
                                       (other, True), (unpacked, True)):
        material['profile'] = selected_profile
        receipt['qualification_key'] = hashlib.sha256(json.dumps(
            material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
        try:
            assert qualified_b1_folder(folder) == (receipt, manifest)
        except ValueError:
            assert not accepted, 'a receipt without gpu was refused'
        else:
            assert accepted, 'a gpu receipt passed without winevulkan.so in the tree'
    material['profile'] = b1
    receipt['qualification_key'] = hashlib.sha256(json.dumps(
        material, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    (folder / 'sce_module/libc.prx').write_bytes(b'corrupted')
    try:
        qualified_b1_folder(folder)
    except ValueError:
        pass
    else:
        raise AssertionError('qualification accepted corrupted title bytes')
    (folder / 'sce_module/libc.prx').write_bytes(b'published')
    receipt['qualification_key'] = '00' * 32
    (folder / 'runtime-qualification.json').write_text(json.dumps(receipt))
    try:
        qualified_b1_folder(folder)
    except ValueError:
        pass
    else:
        raise AssertionError('qualification accepted a forged material digest')
    try:
        (folder / 'wine').mkdir()
        (folder / 'wine/bad.dll').write_bytes(b'bad')
        b1_guard_paths(folder, '/data/homebrew/PPSA99030')
    except ValueError:
        pass
    else:
        raise AssertionError('B1 publication accepted a payload write')
    extra = root / 'lib/wine/x86_64-unix/kerberos.so'
    extra.write_bytes(b'not admitted')
    try:
        runtime_tree_manifest(root, b1)
    except ValueError:
        pass
    else:
        raise AssertionError('B1 tree accepted an additional Unix module')

print('RUNTIME_PROFILE_HOST_PASS root=SEPARATE_TITLE_ONLY guards=NO_PAYLOAD_WRITES modules=EXACT_PROFILE console=NOT_CONTACTED')
