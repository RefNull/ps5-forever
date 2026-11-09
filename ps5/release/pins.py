#!/usr/bin/env python3
"""Single reader of ps5/release/deps.json, the release's pin manifest.

    pins.py value KEY                   print a top-level manifest value
    pins.py download KEY FIELD...       print a download's fields, tab-separated
    pins.py source NAME FIELD           print a source's field
    pins.py check                       host test: every copy that cannot read
                                        deps.json still agrees with it
    pins.py validate-sources ROOT TAG RADV_REVISION
                                        release container: the checkouts under
                                        ROOT are the pinned, unmodified sources

A download KEY is its name without the version suffix ('freetype' for
'freetype-2.15.0'), so callers never repeat a version; FIELD 'version' is that
suffix. An unversioned name is its own key.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

RELEASE = Path(__file__).resolve().parent
PS5 = RELEASE.parent
REPO = PS5.parent
MANIFEST = RELEASE / 'deps.json'
VERSIONED = re.compile(r'(.+?)-(v?\d[\w.-]*)')
TAG = re.compile(r'\d\d\.\d\d\d\.\d\d\d')


def load(path=MANIFEST):
    return json.loads(Path(path).read_text())


def split_name(name):
    match = VERSIONED.fullmatch(name)
    return (match[1], match[2]) if match else (name, None)


def download(manifest, key):
    entries = manifest['downloads']
    exact = [entry for entry in entries if entry['name'] == key]
    found = exact or [entry for entry in entries if split_name(entry['name'])[0] == key]
    if len(found) != 1:
        raise SystemExit(f'deps.json: {len(found)} downloads match {key!r}')
    entry = dict(found[0])
    entry['version'] = split_name(entry['name'])[1]
    return entry


def source(manifest, name):
    found = [entry for entry in manifest['sources'] if entry['name'] == name]
    if len(found) != 1:
        raise SystemExit(f'deps.json: {len(found)} sources named {name!r}')
    return found[0]


def check_wine_lock(manifest):
    """The Wine lock names the same published commit (an earlier release did not)."""
    lock = json.loads((PS5 / 'wine-ps5.lock').read_text())
    wine = source(manifest, 'ps5-wine')
    if lock['commit'] != wine['commit']:
        raise SystemExit('Wine lock does not match deps.json')
    if lock['origin'] != wine['url']:
        raise SystemExit('Wine lock origin does not match deps.json')


def content_version():
    version = json.loads((PS5 / 'title/sce_sys/param.json').read_text())['contentVersion']
    if not TAG.fullmatch(version):
        raise SystemExit(f'param.json contentVersion is not NN.NNN.NNN: {version}')
    return version


def check_tag(tag):
    if not TAG.fullmatch(tag):
        raise SystemExit('tag must be NN.NNN.NNN')
    if content_version() != tag:
        raise SystemExit('tag does not match source contentVersion')


def check(manifest):
    failures = []

    def expect(condition, message):
        if not condition:
            failures.append(message)

    def attempt(function, *arguments):
        try:
            return function(*arguments)
        except SystemExit as error:
            failures.append(str(error))
            return None

    attempt(check_wine_lock, manifest)
    sources = {entry['name']: entry for entry in manifest['sources']}
    expect(len(sources) == len(manifest['sources']), 'duplicate source names')
    for entry in manifest['sources']:
        for field in ('commit', 'original_commit'):
            expect(field not in entry or re.fullmatch(r'[0-9a-f]{40}', entry[field]),
                   f"{entry['name']}: {field} is not a full commit ID")
        expect(re.fullmatch(r'[0-9a-f]{64}', entry['content_sha256']),
               f"{entry['name']}: content_sha256 is not a SHA-256")
    # fetch-sources.py makes depth-1 checkouts, so these must be the pinned commits.
    expect(manifest['sdk_revision'] == sources['PS5_PayloadSDK']['commit'],
           'sdk_revision differs from the PS5_PayloadSDK commit')
    expect(manifest['radv_revision'] == sources['PS5_Mesa']['commit'],
           'radv_revision differs from the PS5_Mesa commit')

    keys = [split_name(entry['name'])[0] for entry in manifest['downloads']]
    for key in sorted({key for key in keys if keys.count(key) > 1}):
        failures.append(f'download key {key!r} is ambiguous')
    for entry in manifest['downloads']:
        expect(re.fullmatch(r'[0-9a-f]{64}', entry['sha256']),
               f"{entry['name']}: sha256 is not a SHA-256")
        expect(entry['url'].startswith('https://'), f"{entry['name']}: URL is not HTTPS")
    # Every download a build script names must resolve.
    used = re.compile(r'''(?:pins\.py"?|\bpin)\s+download\s+([\w.+-]+)'''
                      r'''|pins\.download\(\w+,\s*'([^']+)'\)|--download\s+([\w.+-]+)''')
    for path in sorted([*PS5.glob('*.sh'), *PS5.glob('*/*.sh'), *PS5.glob('*/*.py'),
                        *PS5.glob('*/*/*.py')]):
        if path == Path(__file__).resolve():
            continue
        for match in used.finditer(path.read_text(errors='replace')):
            attempt(download, manifest, next(group for group in match.groups() if group))

    version = attempt(content_version)
    # Wine embeds this in ntdll (build-wine.sh WINE_DESCRIBE). It is the
    # annotated tag upstream publishes on the pinned commit for each upstream
    # release, so `git describe HEAD` of a full clone prints exactly this.
    # This fork versions 01.UUU.FFF on upstream 01.000.UUU: the tag must name
    # that upstream base.
    describe = sources['ps5-wine'].get('describe', '')
    parts = (version or '').split('.')
    base = f'{parts[0]}.000.{parts[1]}' if len(parts) == 3 else ''
    expect(re.fullmatch(rf'wine-\d+\.\d+-\d+-forever-{re.escape(base)}', describe),
           f'ps5-wine describe {describe!r} is not the wine-*-forever-{base} '
           f'upstream tag for {version}')
    workflow = (REPO / '.github/workflows/release.yml').read_text()
    default = re.search(r"version:\n(?:\s+\w+:.*\n)*?\s+default: '([^']*)'", workflow)
    expect(default and default[1] == version,
           'release.yml default version differs from param.json contentVersion')
    actions = {}
    for action, revision in re.findall(r'uses: ([\w./-]+)@(\S+)', workflow):
        expect(re.fullmatch(r'[0-9a-f]{40}', revision), f'{action} is not pinned to a commit')
        expect(actions.setdefault(action, revision) == revision, f'{action} is pinned twice')

    # The image is built before any source exists, so it repeats these pins.
    dockerfile = (RELEASE / 'Dockerfile').read_text()
    llvm = download(manifest, 'llvm-mingw')
    expect(re.search(rf'^\s+{re.escape(llvm["url"])} \\$', dockerfile, re.M),
           'Dockerfile llvm-mingw URL differs from deps.json')
    expect(f"echo '{llvm['sha256']}  /tmp/llvm-mingw.tar.xz'" in dockerfile,
           'Dockerfile llvm-mingw SHA-256 differs from deps.json')
    toolchain = re.findall(r'--default-toolchain (\S+)', dockerfile)
    expect(toolchain == [manifest['rust_toolchain']],
           'Dockerfile Rust toolchain differs from deps.json')

    # Licence records must name the pinned archive or commit they came from.
    provenance = json.loads((RELEASE / 'licenses/provenance.json').read_text())['files']
    by_url = {entry['url']: entry for entry in manifest['downloads']}
    commits = {}
    for entry in manifest['sources']:
        commits.setdefault(entry['url'], set()).add(entry['commit'])
    for asset, record in provenance.items():
        url, revision = record.get('url'), record.get('revision')
        if url in by_url:
            pinned = by_url[url]
            expect(record.get('archive_sha256', pinned['sha256']) == pinned['sha256'],
                   f'provenance {asset}: archive_sha256 differs from {pinned["name"]}')
            expect(revision in (None, pinned.get('commit')),
                   f'provenance {asset}: revision differs from {pinned["name"]}')
            if Path(asset).name == pinned['name']:
                expect(record.get('sha256') == pinned['sha256'],
                       f'provenance {asset}: sha256 differs from {pinned["name"]}')
        elif url in commits:
            expect(revision in commits[url], f'provenance {asset}: revision {revision} is not pinned')
    miniz = json.loads((PS5 / 'addon-worker/third_party/miniz/SOURCE.json').read_text())
    record = provenance['miniz/LICENSE']
    expect((record['url'], record['revision'], record['sha256']) ==
           (miniz['repository'], miniz['commit'], miniz['files']['LICENSE']),
           'provenance miniz/LICENSE differs from the vendored SOURCE.json')
    # PS5_Vulkan's verbatim notice names the archives its setup script verifies.
    notice = (RELEASE / 'licenses/PS5_Vulkan/NOTICE.txt').read_text()
    for key in ('payload-sdk', 'native-zlib'):
        expect(download(manifest, key)['sha256'] in notice,
               f'PS5_Vulkan NOTICE.txt does not name the {key} SHA-256')

    for failure in failures:
        print(f'FAIL {failure}')
    if failures:
        raise SystemExit(f'test-pins: {len(failures)} pin(s) disagree with deps.json')
    print('PASS test-pins')


def validate_sources(manifest, root, tag, radv_revision):
    root = Path(root)
    check_tag(tag)
    # Bind-mounted checkouts retain the host UID; trust only the manifest paths
    # in this disposable container, not arbitrary repositories.
    for entry in [manifest['project'], *manifest['sources']]:
        subprocess.run(['git', 'config', '--global', '--add', 'safe.directory',
                        str(root / entry['path'])], check=True)

    def git(path, *args):
        return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()

    for entry in manifest['sources']:
        path = root / entry['path']
        actual = git(path, 'rev-parse', 'HEAD')
        records = subprocess.check_output(['git', '-C', str(path), 'ls-files', '--stage', '-z'])
        fingerprint = hashlib.sha256(b'\0'.join(
            row for row in records.split(b'\0') if row and not row.startswith(b'160000 '))).hexdigest()
        if fingerprint != entry['content_sha256']:
            raise SystemExit(f"{entry['name']}: source content fingerprint mismatch: {fingerprint}")
        if git(path, 'status', '--porcelain', '--untracked-files=no', '--ignore-submodules'):
            raise SystemExit(f"{entry['name']}: modified tracked sources")
        print(f"BUILD_SOURCE {entry['name']} {actual}")
    for name, revision in (('PS5_PayloadSDK', manifest['sdk_revision']),
                           ('PS5_Mesa', radv_revision)):
        git(root / source(manifest, name)['path'], 'cat-file', '-e', revision + '^{commit}')
    check_wine_lock(manifest)


def main(argv):
    manifest = load()
    command, arguments = (argv[0], argv[1:]) if argv else (None, [])
    if command == 'value' and len(arguments) == 1:
        print(manifest[arguments[0]])
    elif command == 'download' and len(arguments) >= 2:
        entry = download(manifest, arguments[0])
        print('\t'.join(str(entry[field]) for field in arguments[1:]))
    elif command == 'source' and len(arguments) == 2:
        print(source(manifest, arguments[0])[arguments[1]])
    elif command == 'check' and not arguments:
        check(manifest)
    elif command == 'validate-sources' and len(arguments) == 3:
        validate_sources(manifest, *arguments)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
