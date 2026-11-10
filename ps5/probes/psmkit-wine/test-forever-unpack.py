#!/usr/bin/env python3
"""Unpack mode (forever-unpack.h) against zips made here: usage HARNESS."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

HARNESS = Path(sys.argv[1]).resolve()
LIMIT = 64 * 1024 * 1024


def make_zip(path, members, compression=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(path, 'w', compression) as archive:
        for name, data in members:
            archive.writestr(name, data)
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unpack(zip_path, sha, member, destination):
    run = subprocess.run([HARNESS, zip_path, sha, member, destination],
                         capture_output=True, text=True, check=False)
    assert run.returncode in (0, 1), run
    return run.returncode == 0, run.stdout.strip()


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    drive_c = root / 'prefix/drive_c'
    drive_c.mkdir(parents=True)
    launcher = drive_c / 'Program Files (x86)/Steam/steam.exe'
    part = launcher.with_name('steam.exe.part')
    payload = b'MZ' + os.urandom(300 * 1024)
    good = drive_c / 'steam_win64.zip'

    def refused(sha, member, step, zip_path=good):
        ok, output = unpack(zip_path, sha, member, launcher)
        assert not ok and step in output, (member, step, output)
        assert not launcher.exists() and not part.exists(), output

    # The member's parents are created; the result is the member, nothing else.
    sha = make_zip(good, [('steam.exe', payload)])
    ok, output = unpack(good, sha, 'steam.exe', launcher)
    assert ok, output
    assert launcher.read_bytes() == payload and not part.exists()
    assert sorted(path.name for path in launcher.parent.iterdir()) == ['steam.exe']
    # A second unpack replaces the published file.
    payload = b'MZ' + os.urandom(1024)
    sha = make_zip(good, [('steam.exe', payload)], zipfile.ZIP_STORED)
    assert unpack(good, sha, 'steam.exe', launcher)[0] and launcher.read_bytes() == payload
    launcher.unlink()

    refused('0' * 64, 'steam.exe', 'download SHA-256 mismatch')
    refused(sha.upper(), 'steam.exe', 'download SHA-256 mismatch')
    refused(sha[:63], 'steam.exe', 'download SHA-256 mismatch')
    refused(sha, 'other.exe', 'zip does not hold exactly the expected file')

    sha = make_zip(good, [('steam.exe', payload), ('extra.txt', b'x')])
    refused(sha, 'steam.exe', 'zip does not hold exactly the expected file')
    sha = make_zip(good, [('extra.txt', b'x'), ('steam.exe', payload)])
    refused(sha, 'steam.exe', 'zip does not hold exactly the expected file')
    sha = make_zip(good, [])
    refused(sha, 'steam.exe', 'zip does not hold exactly the expected file')
    for name in ('../steam.exe', 'Steam/steam.exe', '/steam.exe', 'C:\\steam.exe',
                 '..\\steam.exe', 'steam.exe/'):
        sha = make_zip(good, [(name, payload)])
        refused(sha, 'steam.exe', 'zip does not hold exactly the expected file')
        # The member setting is a plain name too, whatever the zip holds.
        refused(sha, name, 'invalid unpack settings')
    sha = make_zip(good, [('..', payload)])
    refused(sha, '..', 'invalid unpack settings')

    # 64 MiB is the cap for both the zip and its member.
    sha = make_zip(good, [('steam.exe', bytes(LIMIT + 1))])
    assert good.stat().st_size < LIMIT
    refused(sha, 'steam.exe', 'zip file is too large')
    sha = make_zip(good, [('steam.exe', bytes(LIMIT))])
    assert unpack(good, sha, 'steam.exe', launcher)[0] and launcher.stat().st_size == LIMIT
    launcher.unlink()
    huge = drive_c / 'huge.zip'
    with huge.open('wb') as file:
        file.truncate(LIMIT + 1)
    refused(hashlib.sha256(huge.read_bytes()).hexdigest(), 'steam.exe', 'download is too large', huge)
    huge.unlink()

    # Hashed bytes that are not a zip, or a zip whose data fails its CRC.
    good.write_bytes(payload)
    refused(hashlib.sha256(payload).hexdigest(), 'steam.exe', 'download is not a zip')
    make_zip(good, [('steam.exe', payload)], zipfile.ZIP_STORED)
    damaged = bytearray(good.read_bytes())
    damaged[damaged.index(payload[:64]) + 10] ^= 0xff
    good.write_bytes(damaged)
    refused(hashlib.sha256(damaged).hexdigest(), 'steam.exe', 'zip file is damaged')
    refused(sha, 'steam.exe', 'reading the download', drive_c / 'missing.zip')

print('forever unpack: one-member zip published; sha, members, names, sizes and CRC refused PASS')
