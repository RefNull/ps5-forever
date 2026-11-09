#!/usr/bin/env python3
"""Deploy and run the B1 launcher title on the PS5 over FTP.

  ps5/deploy-wine.py upload-title DIR  ps5/title/build.sh's DIR -> /data/homebrew/$TITLE_ID
      (eboot.bin, sce_sys, sce_module beside wine/; nothing is deleted)
  ps5/deploy-wine.py title launch|kill|restart
      start or close the title through ../../PS5_Vulkan's resident agent ps5vkctl
      (port 9111), which acts only on the title id it is given; it is sent to
      the ELF loader first if it does not answer
  ps5/deploy-wine.py title-backup RUN --paths-from BEFORE.json
      require title count=0, disable FTP SELF conversion, and make a fresh
      byte/hash/absence backup at ps5/build/runs/RUN-backup/before.json
  ps5/deploy-wine.py title-restore RUN
      stop the title, restore that backup byte-exactly, delete originally absent
      disposable trees through a saved root-constrained inventory, and require
      title count=0
  ps5/deploy-wine.py title-root-upload | b1-title-run RUN | b1-title-install RUN
      upload, launch once, or install the qualified B1 title (build/title-psmkit-wine-b1)
  ps5/deploy-wine.py prefix-permissions --log FILE
      open a prefix tree to every console process

Host: PS5_HOST, FTP_PORT (2121), LOADER_PORT (9021), PS5VKCTL_PORT (9111).
"""
import argparse
import hashlib
import json
import posixpath
import signal
import os
import subprocess
import socket
import sys
import time
from ftplib import FTP, error_perm, error_reply
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "probes/psmkit-wine"))
from runtime_profiles import (
    profile, b1_title_files, b1_guard_paths, qualified_b1_folder, file_identity,
)
# TITLE_ID from ps5/paths.sh: Wine lives in the launcher title's folder.
TITLE_ID = next(line.split("=", 1)[1].strip() for line in (HERE / "paths.sh").read_text().splitlines()
                if line.startswith("TITLE_ID="))
REMOTE = f"/data/homebrew/{TITLE_ID}/wine"
HOST = os.environ.get("PS5_HOST", "10.0.1.41")
FTP_PORT = int(os.environ.get("FTP_PORT", "2121"))
LOADER_PORT = int(os.environ.get("LOADER_PORT", "9021"))
RUNS = HERE / "build/runs"

ENV = [
    f"WINEPREFIX={REMOTE}/prefix",
    f"HOME={REMOTE}/home",
    "USER=ps5",
    "PATH=/usr/bin:/bin",
    # winedbg: a crash must not start a debugger nobody can use, which on
    # the console multiplied processes until kernel page tables ran out.
    # msv1_0 (NTLM) and kerberos: Windows domain logins nothing here uses;
    # neither is built (no ntlm_auth, --without-krb5), and schannel's start
    # loaded each, failed and unloaded it, then hit a SceLibc heap error.
    # tabtip.exe, which explorer starts on every desktop for a touch keyboard,
    # queries the focus through UI Automation, which turns on Chromium's
    # accessibility; those queries came right before each breakpoint crash of
    # Battle.net's renderer that reloaded the page (2026-10-04). The title
    # has its own keyboard.
    "WINEDLLOVERRIDES=winemenubuilder.exe=d;mscoree=d;mshtml=d;winedbg.exe=d;msv1_0=d;kerberos=d;tabtip.exe=d",
]
def connect():
    ftp = FTP()
    ftp.connect(HOST, FTP_PORT, timeout=30)
    ftp.login()
    return ftp


def mkdirs(ftp, path, made):
    parts = path.strip("/").split("/")
    for i in range(1, len(parts) + 1):
        p = "/" + "/".join(parts[:i])
        if p in made:
            continue
        try:
            ftp.mkd(p)
        except error_perm:
            pass
        made.add(p)


TITLE_ROOT = f"/data/homebrew/{TITLE_ID}"
TITLE_LAST = ("eboot.bin", "sce_sys/param.json")
CONTROL_PORT = int(os.environ.get("PS5VKCTL_PORT", "9111"))
CONTROL_ELF = HERE.parent.parent.parent / "PS5_Vulkan/build/ps5vkctl/ps5vkctl.elf"


def upload_title(args):
    """Upload a built title folder (ps5/title/build.sh's OUT_DIR) into the
    title's folder, beside wine/: each file under a temporary name, renamed
    once complete, eboot.bin and param.json last. Nothing is deleted."""
    local = Path(args.folder)
    files = getattr(args, "files", None)
    if files is None:
        files = sorted(p.relative_to(local).as_posix() for p in local.rglob("*") if p.is_file())
    if not set(TITLE_LAST) <= set(files):
        raise SystemExit(f"{local} has no {' or '.join(TITLE_LAST)}")
    ftp = connect()
    made = set()
    for rel in [f for f in files if f not in TITLE_LAST] + list(TITLE_LAST):
        remote = f"{TITLE_ROOT}/{rel}"
        temporary = remote.rsplit("/", 1)[0] + "/.upload-" + rel.rsplit("/", 1)[-1]
        mkdirs(ftp, remote.rsplit("/", 1)[0], made)
        with (local / rel).open("rb") as f:
            ftp.storbinary(f"STOR {temporary}", f)
        try:
            ftp.delete(remote)
        except (error_perm, error_reply):
            # missing: error_perm; the console's server answers a delete with
            # 226, which ftplib raises as an unexpected reply
            pass
        ftp.rename(temporary, remote)
    ftp.quit()
    print(f"uploaded {len(files)} title files to {TITLE_ROOT}")


def control(command):
    """One command to ../../PS5_Vulkan's resident agent, ps5vkctl; it is sent to
    the ELF loader first when it does not answer (once per console boot)."""
    def ask():
        with socket.create_connection((HOST, CONTROL_PORT), timeout=30) as s:
            s.sendall((command + "\n").encode())
            s.shutdown(socket.SHUT_WR)
            return b"".join(iter(lambda: s.recv(4096), b"")).decode(errors="replace").strip()
    try:
        return ask()
    except OSError:
        if not CONTROL_ELF.exists():
            raise SystemExit(f"ps5vkctl does not answer and {CONTROL_ELF} is missing")
        s = socket.create_connection((HOST, LOADER_PORT), timeout=10)
        s.sendall(CONTROL_ELF.read_bytes())
        s.close()
        time.sleep(3)
        return ask()


def title(args):
    print(control(f"{args.action} {TITLE_ID}"))

def raw_connect():
    """FTP connection returning raw signed SELF bytes, never decoded files."""
    ftp = connect()
    # SELF toggles the server's decoding of signed SELF files. Its default
    # differs by console (on with 5.50's server, off with 11.60's): toggle
    # until it reports disabled.
    reply = ftp.sendcmd("SELF")
    if "enabled" in reply:
        reply = ftp.sendcmd("SELF")
    if not reply.startswith("226") and "disabled" not in reply:
        ftp.close()
        raise RuntimeError(f"SELF conversion was not disabled: {reply}")
    return ftp, reply


def ftp_bytes(ftp, path):
    from io import BytesIO
    data = BytesIO()
    ftp.retrbinary(f"RETR {path}", data.write)
    return data.getvalue()


def title_backup(args):
    """Fresh raw backup of an existing run's guarded title path set."""
    guards = getattr(args, "guard_paths", None)
    source = {"paths": guards} if guards is not None else json.loads(Path(args.paths_from).read_text())
    source_paths = source.get("paths")
    if not isinstance(source_paths, dict) or not source_paths:
        raise SystemExit(f"{args.paths_from}: missing non-empty paths object")
    source_paths = dict(source_paths)
    added_paths = set(source_paths) if guards is not None else set()
    procs = control("procs")
    if "count=0" not in procs:
        raise SystemExit(f"title is not idle: {procs}")
    backup = RUNS / f"{args.run}-backup"
    if backup.exists():
        raise SystemExit(f"backup already exists: {backup}")
    backup.mkdir(parents=True)
    ftp, reply = raw_connect()
    paths = {}
    drift = []
    try:
        for remote, previous in source_paths.items():
            if not remote.startswith(TITLE_ROOT + "/") or ".." in Path(remote).parts:
                raise RuntimeError(f"guard path escapes title root: {remote}")
            if previous.get("kind") == "directory":
                lines = []
                try:
                    ftp.retrlines(f"LIST {remote}", lines.append)
                    present = True
                except error_perm:
                    present = False
                paths[remote] = {"present": present, "kind": "directory"}
            else:
                try:
                    data = ftp_bytes(ftp, remote)
                except error_perm as error:
                    paths[remote] = {"present": False, "error": str(error)}
                else:
                    relative = remote.removeprefix(TITLE_ROOT + "/")
                    local = backup / relative
                    local.parent.mkdir(parents=True, exist_ok=True)
                    local.write_bytes(data)
                    paths[remote] = {
                        "present": True, "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "backup": str(local),
                    }
            actual = paths[remote]
            if remote not in added_paths and (
                    actual.get("present") != previous.get("present") or (
                        actual.get("present") and previous.get("sha256") and
                        actual.get("sha256") != previous.get("sha256"))):
                drift.append(remote)
    finally:
        ftp.quit()
    before = {
        "format": 1, "host": HOST, "run": args.run, "raw_ftp_reply": reply,
        "procs": procs, "paths": paths, "diff_vs_source": drift,
        "restore_mode": "TITLE_ONLY" if getattr(args, "title_only", False) else "PAYLOAD_AND_TITLE",
    }
    (backup / "before.json").write_text(json.dumps(before, indent=2, sort_keys=True) + "\n")
    if drift:
        raise SystemExit(f"fresh backup differs from source at: {drift}")
    print(f"title-backup: {backup} present={sum(v['present'] for v in paths.values())} "
          f"absent={sum(not v['present'] for v in paths.values())}")


def _delete_reply_ok(ftp, path):
    try:
        ftp.delete(path)
    except error_reply as error:
        if not str(error).startswith("226"):
            raise


def _inventory_tree(ftp, root):
    files, directories, seen, stack = [], [], set(), [root]
    while stack:
        path = posixpath.normpath(stack.pop())
        if path != root and not path.startswith(root + "/"):
            raise RuntimeError(f"inventory escaped root {root}: {path}")
        if path in seen:
            continue
        seen.add(path)
        directories.append(path)
        lines = []
        try:
            ftp.retrlines(f"LIST {path}", lines.append)
        except error_perm:
            continue
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) != 9:
                raise RuntimeError(f"unparsed LIST row: {line}")
            name = parts[8]
            if name in (".", ".."):
                continue
            child = posixpath.normpath(path + "/" + name)
            if not child.startswith(root + "/"):
                raise RuntimeError(f"inventory child escaped root {root}: {child}")
            (stack if parts[0].startswith("d") else files).append(child)
    return sorted(files), sorted(directories, key=lambda p: (p.count("/"), p), reverse=True)


def title_restore(args):
    """Restore a title-backup and its original absences exactly."""
    backup = RUNS / f"{args.run}-backup"
    before = json.loads((backup / "before.json").read_text())
    paths = before["paths"]
    title_only = getattr(args, "title_only", False) or before.get("restore_mode") == "TITLE_ONLY"
    if "count=0" not in control("procs"):
        control(f"kill {TITLE_ID}")
    deadline = time.monotonic() + 30
    while "count=0" not in control("procs"):
        if time.monotonic() >= deadline:
            raise RuntimeError("title did not stop before restoration")
        time.sleep(0.25)
    ftp, reply = raw_connect()
    restored, absent, plans = {}, [], {}
    try:
        for remote, metadata in paths.items():
            if metadata.get("kind") == "directory":
                continue
            if metadata["present"]:
                data = Path(metadata["backup"]).read_bytes()
                if title_only and remote == REMOTE + "/boot.txt":
                    try:
                        unchanged = ftp_bytes(ftp, remote) == data
                    except error_perm:
                        unchanged = False
                    if unchanged:
                        continue
                ftp.storbinary(f"STOR {remote}", __import__("io").BytesIO(data), blocksize=1 << 16)
            elif metadata.get("kind") != "directory":
                try:
                    _delete_reply_ok(ftp, remote)
                except error_perm:
                    pass
                absent.append(remote)
        for root, metadata in paths.items():
            if metadata.get("kind") != "directory" or metadata["present"]:
                continue
            files, directories = _inventory_tree(ftp, root)
            plans[root] = {"files": files, "directories_deepest_first": directories}
            for path in files:
                try:
                    _delete_reply_ok(ftp, path)
                except error_perm:
                    pass
            for path in directories:
                try:
                    ftp.rmd(path)
                except (error_perm, error_reply):
                    pass
            absent.append(root)
        for remote, metadata in paths.items():
            if metadata.get("kind") == "directory" and metadata["present"]:
                ftp.retrlines(f"LIST {remote}", lambda line: None)
            elif metadata["present"]:
                data = ftp_bytes(ftp, remote)
                digest = hashlib.sha256(data).hexdigest()
                if digest != metadata["sha256"] or len(data) != metadata["bytes"]:
                    raise RuntimeError(f"restored bytes differ: {remote}")
                restored[remote] = {"sha256": digest, "bytes": len(data)}
            elif metadata.get("kind") == "directory":
                try:
                    lines = []
                    ftp.retrlines(f"LIST {remote}", lines.append)
                    raise RuntimeError(f"directory remains after restore: {remote}")
                except error_perm:
                    pass
            else:
                try:
                    ftp_bytes(ftp, remote)
                    raise RuntimeError(f"file remains after restore: {remote}")
                except error_perm:
                    pass
        # Home-screen copies an install replaced (b1-title-install): back as they were.
        art = backup / "home-art.json"
        for remote, metadata in (json.loads(art.read_text()) if art.exists() else {}).items():
            data = Path(metadata["backup"]).read_bytes()
            ftp.storbinary(f"STOR {remote}", __import__("io").BytesIO(data), blocksize=1 << 16)
            if hashlib.sha256(ftp_bytes(ftp, remote)).hexdigest() != metadata["sha256"]:
                raise RuntimeError(f"restored home-screen art differs: {remote}")
            restored[remote] = {"sha256": metadata["sha256"], "bytes": len(data)}
    finally:
        ftp.quit()
    (backup / "disposable-delete-plan.json").write_text(
        json.dumps({"roots": plans}, indent=2, sort_keys=True) + "\n")
    procs = control("procs")
    if "count=0" not in procs:
        raise RuntimeError(f"title is not idle after restore: {procs}")
    receipt = {
        "format": 1, "run": args.run, "raw_ftp_reply": reply,
        "restored_exact": restored, "original_absences_restored": absent,
        "procs": procs,
    }
    (backup / "final-restoration.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"title-restore: {backup} restored={len(restored)} absent={len(absent)} {procs}")


def ftp_identity(ftp, remote):
    digest = hashlib.sha256()
    size = 0
    def consume(data):
        nonlocal size
        size += len(data)
        digest.update(data)
    ftp.retrbinary(f"RETR {remote}", consume, blocksize=1 << 16)
    return {"bytes": size, "sha256": digest.hexdigest()}


def title_root_upload(args):
    """Upload only the qualified, separately rooted B1 Wine tree."""
    folder = Path(args.folder).resolve()
    qualification, manifest = qualified_b1_folder(folder)
    expected_files = {item["path"]: {"bytes": item["size"], "sha256": item["sha256"]}
                      for item in qualification["material"]["title_root"]}
    root = profile("b1")["remote_root"]
    if root != TITLE_ROOT + "/wine-title":
        raise SystemExit("B1 profile is only authorized for PPSA99030")
    procs = control("procs")
    if "count=0" not in procs:
        raise SystemExit(f"title is not idle: {procs}")
    ftp, reply = raw_connect()
    verified = {}
    try:
        rows = []
        ftp.retrlines(f"LIST {TITLE_ROOT}", rows.append)
        entries = [row.split(None, 8) for row in rows]
        if any(len(entry) != 9 for entry in entries):
            raise RuntimeError("unparsed title-root LIST reply")
        existing = [entry for entry in entries if entry[8].split(" -> ", 1)[0] == "wine-title"]
        if existing:
            if not args.replace:
                raise SystemExit(f"title root already exists: {root}; use --replace deliberately")
            if len(existing) != 1 or not existing[0][0].startswith("d"):
                raise RuntimeError(f"title root is not a real directory: {root}")
            files, directories = _inventory_tree(ftp, root)
            for remote in files:
                _delete_reply_ok(ftp, remote)
            for directory in directories:
                ftp.rmd(directory)
        made = set()
        files = list(manifest["files"]) + ["title-root-manifest.json"]
        for relative in files:
            local = folder / "wine-title" / relative
            remote = root + "/" + relative
            mkdirs(ftp, posixpath.dirname(remote), made)
            with local.open("rb") as source:
                ftp.storbinary(f"STOR {remote}", source, blocksize=1 << 16)
            identity = ftp_identity(ftp, remote)
            expected = expected_files[relative]
            if identity != expected:
                raise RuntimeError(f"title root upload bytes differ: {remote}")
            verified[remote] = identity
    finally:
        ftp.quit()
    receipt = {"format": 1, "remote_root": root, "raw_ftp_reply": reply,
               "qualification_digest": qualification["qualification_key"],
               "pe_dll_counts": manifest["pe_dll_counts"], "verified": verified,
               "payload_install": "UNTOUCHED"}
    path = HERE / "build/title-root-upload.json"
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"title-root-upload: {root} verified={len(verified)} pe_dlls={manifest['pe_dll_counts']} receipt={path}")


REGISTRY = ("system.reg", "user.reg", "userdef.reg")


def b1_preflight(folder, require_battlenet=True):
    """Read-only checks before B1 title files are published: a qualified folder
    for PPSA99030, an idle title, the uploaded root matching the qualifier,
    and an initialized prefix (update timestamp, registries) holding Battle.net
    unless require_battlenet is false (the title then installs it itself)."""
    qualification, manifest = qualified_b1_folder(folder)
    selected = qualification["material"]["profile"]
    if selected["remote_root"] != TITLE_ROOT + "/wine-title":
        raise SystemExit("B1 profile is only authorized for PPSA99030")
    guards = b1_guard_paths(folder, TITLE_ROOT)
    files = b1_title_files(folder)
    material = qualification["material"]
    expected_title = {item["path"]: {"bytes": item["size"], "sha256": item["sha256"]}
                      for item in material["parent_title"]}
    expected_title["psmkit-wine-runtime-child.bin"] = {
        "bytes": material["native_child"]["fself_size"],
        "sha256": material["native_child"]["fself_sha256"],
    }
    expected_title["runtime-qualification.json"] = file_identity(folder / "runtime-qualification.json")
    # The root must already be uploaded and match the qualifier; the
    # installed application prefix is never booted or updated.
    procs = control("procs")
    if "count=0" not in procs:
        raise SystemExit(f"title is not idle: {procs}")
    ftp, _ = raw_connect()
    try:
        remote_manifest = ftp_bytes(ftp, selected["remote_root"] + "/title-root-manifest.json")
        if remote_manifest != (folder / "wine-title/title-root-manifest.json").read_bytes():
            raise RuntimeError("B1 root manifest differs; run title-root-upload --replace")
        for relative, identity in manifest["files"].items():
            remote = selected["remote_root"] + "/" + relative
            if ftp_identity(ftp, remote) != identity:
                raise RuntimeError(f"B1 root bytes differ: {remote}")
        prefix = selected["prefix"].replace("/app0/", TITLE_ROOT + "/", 1)
        try:
            timestamp = ftp_bytes(ftp, prefix + "/.update-timestamp").strip()
        except error_perm:
            if require_battlenet:
                raise RuntimeError(f"no initialized B1 prefix at {prefix}")
            timestamp = None  # the app's first start creates the prefix itself
        if timestamp is not None:
            if not timestamp or not (timestamp.isdigit() or timestamp.startswith(b"disable")):
                raise RuntimeError("installed B1 prefix has no valid update timestamp")
            for name in REGISTRY:
                if not ftp_bytes(ftp, prefix + "/" + name).startswith(b"WINE REGISTRY Version 2\n"):
                    raise RuntimeError(f"installed B1 prefix has invalid {name}")
        for leaf in ("Battle.net Launcher.exe", "Battle.net.exe") if require_battlenet else ():
            executable = prefix + "/drive_c/Program Files (x86)/Battle.net/" + leaf
            if not ftp_bytes(ftp, executable).startswith(b"MZ"):
                raise RuntimeError(f"installed Battle.net executable is missing or invalid: {executable}")
    finally:
        ftp.quit()
    return qualification, selected, guards, files, expected_title


def b1_verify_published(files, expected_title):
    """Digest every published title file against the qualifier."""
    ftp, reply = raw_connect()
    deployed = {}
    try:
        for relative in files:
            remote = TITLE_ROOT + "/" + relative
            identity = ftp_identity(ftp, remote)
            if identity != expected_title[relative]:
                raise RuntimeError(f"B1 published file differs: {remote}")
            deployed[remote] = identity
    finally:
        ftp.quit()
    return deployed, reply

# The home screen reads the copies ShadowMountPlus made of sce_sys when it
# registered the title (once), not the title's own folder. The app installs
# its icon there itself, through the write broker (WB_ICON_MAGIC,
# psmkit-wine/title-icon.h); this replaces the backgrounds after a title
# install. Only existing copies are replaced.
HOME_ART = {
    "pic0.dds": ("/user/app/{id}/sce_sys/pic0.dds", "/user/appmeta/{id}/pic0.dds"),
    "pic1.dds": ("/user/app/{id}/sce_sys/pic1.dds", "/user/appmeta/{id}/pic1.dds"),
}


def install_home_art(run, art):
    """Replace the home screen's existing copies of each name in art (bytes),
    keeping every replaced copy in the run's backup (home-art.json), which
    title-restore puts back. Returns {remote: sha256 written}."""
    from io import BytesIO
    backup = RUNS / f"{run}-backup"
    store = backup / "home-art"
    store.mkdir(parents=True, exist_ok=True)
    record, written = {}, {}
    ftp, _ = raw_connect()
    try:
        for name, data in art.items():
            for pattern in HOME_ART[name]:
                remote = pattern.format(id=TITLE_ID)
                try:
                    old = ftp_bytes(ftp, remote)
                except error_perm:
                    continue
                saved = store / remote.strip("/").replace("/", "__")
                saved.write_bytes(old)
                record[remote] = {"backup": str(saved), "sha256": hashlib.sha256(old).hexdigest()}
                (backup / "home-art.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
                ftp.storbinary(f"STOR {remote}", BytesIO(data), blocksize=1 << 16)
                digest = hashlib.sha256(ftp_bytes(ftp, remote)).hexdigest()
                if digest != hashlib.sha256(data).hexdigest():
                    raise RuntimeError(f"home-screen art differs after upload: {remote}")
                written[remote] = digest
    finally:
        ftp.quit()
    return written


def b1_title_install(args):
    """Make the qualified B1 title PPSA99030's program: the home-screen icon then
    runs Battle.net inside the title, on the title's own direct memory, instead
    of the launcher title's payload Wine, whose memory comes from the console's
    and froze it twice (2026-10-04, 2026-10-08). Nothing is launched and
    nothing is restored afterwards; `title-restore RUN` puts the previous title
    files and home-screen art back. A prefix without Battle.net is accepted:
    the title then downloads and runs Blizzard's installer itself."""
    folder = Path(args.folder).resolve()
    qualification, selected, guards, files, expected_title = b1_preflight(folder, require_battlenet=False)
    prefix = selected["prefix"].replace("/app0/", TITLE_ROOT + "/", 1)
    home = selected["home"].replace("/app0/", TITLE_ROOT + "/", 1)
    # The app owns its prefix files, Battle.net's SwiftShader manifest, the
    # first-start prefix in /data/forever (forever-setup.h) and its home-screen
    # icon (title-icon.h): install the title and the backgrounds only.
    backup = RUNS / f"{args.run}-backup"
    title_backup(argparse.Namespace(run=args.run, guard_paths=guards, title_only=True))
    try:
        upload_title(argparse.Namespace(folder=str(folder), files=files))
        deployed, reply = b1_verify_published(files, expected_title)
        art = {name: (folder / "sce_sys" / name).read_bytes() for name in ("pic0.dds", "pic1.dds")
               if (folder / "sce_sys" / name).is_file()}
        home_art = install_home_art(args.run, art)
    except BaseException:
        title_restore(argparse.Namespace(run=args.run, title_only=True))
        raise
    (backup / "installed.json").write_text(json.dumps({
        "format": 1, "raw_ftp_reply": reply, "files": deployed,
        "qualification_digest": qualification["qualification_key"],
        "prefix": prefix, "home": home,
        "home_art": home_art,
        "revert": f"deploy-wine.py title-restore {args.run}",
    }, indent=2, sort_keys=True) + "\n")
    print(f"b1-title-install: {len(deployed)} title files, user data {prefix}, "
          f"{len(home_art)} home-screen copies replaced; the home screen shows new art after a restart; "
          f"revert with: deploy-wine.py title-restore {args.run}")


def b1_title_run(args):
    """Back up all title writes, launch once, and restore without payload Wine."""
    if args.timeout <= 0 or args.exit_grace < 0:
        raise SystemExit("B1 needs a positive bounded timeout and nonnegative exit grace")
    folder = Path(args.folder).resolve()
    qualification, selected, guards, files, expected_title = b1_preflight(folder)
    backup = RUNS / f"{args.run}-backup"
    launched = False
    launch_result = None
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"B1 transaction interrupted by signal {signum}")
    previous_handlers = {number: signal.getsignal(number) for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    for number in previous_handlers:
        signal.signal(number, interrupted)
    try:
        title_backup(argparse.Namespace(run=args.run, guard_paths=guards, title_only=True))
        before = json.loads((backup / "before.json").read_text())
        try:
            upload_title(argparse.Namespace(folder=str(folder), files=files))
            deployed, reply = b1_verify_published(files, expected_title)
            ftp, _ = raw_connect()
            try:
                boot = REMOTE + "/boot.txt"
                boot_metadata = before["paths"][boot]
                if boot_metadata["present"]:
                    if ftp_identity(ftp, boot) != {key: boot_metadata[key] for key in ("bytes", "sha256")}:
                        raise RuntimeError("boot.txt changed before B1 launch")
                else:
                    try:
                        ftp_bytes(ftp, boot)
                    except error_perm:
                        pass
                    else:
                        raise RuntimeError("boot.txt appeared before B1 launch")
            finally:
                ftp.quit()
            (backup / "deployed-all.json").write_text(json.dumps({
                "format": 1, "raw_ftp_reply": reply, "files": deployed,
                "qualification_digest": qualification["qualification_key"],
                "guards": "MATCHED_BEFORE_LAUNCH", "prefix": "EXISTING_NO_WINEBOOT",
                "payload_install": "UNTOUCHED", "title_root": selected["remote_root"],
            }, indent=2, sort_keys=True) + "\n")
            command = [
                sys.executable, str(HERE.parent.parent.parent / "PS5_Vulkan/tools/run-title.py"),
                TITLE_ID, "--until", "b1 parent result=WINE_EXIT wine_exit=" +
                (f"|{args.until}" if args.until else ""),
                "--timeout", str(args.timeout), "--exit-grace", str(args.exit_grace),
                "--output", str(RUNS / f"{args.run}.klog"),
            ]
            if args.echo:
                command += ["--echo", args.echo]
            launched = True
            result = subprocess.run(command)
            launch_result = {"command": command, "returncode": result.returncode, "title_launches": 1,
                             "qualification_digest": qualification["qualification_key"]}
        finally:
            # Ordinary termination signals must not interrupt byte restoration.
            for number in previous_handlers:
                signal.signal(number, signal.SIG_IGN)
            try:
                title_restore(argparse.Namespace(run=args.run, title_only=True))
            finally:
                if launch_result is None:
                    launch_result = {"title_launches": int(launched), "returncode": None,
                                     "failure": "transaction ended before run-title returned"}
                (backup / "launch.json").write_text(json.dumps(launch_result, indent=2) + "\n")
    finally:
        for number, handler in previous_handlers.items():
            signal.signal(number, handler)
    if launch_result["returncode"]:
        raise SystemExit(launch_result["returncode"])


def prefix_permissions(args):
    """Open a prefix tree to every console process, as Wine now creates it.

    Payload Wine (uid 0) and a title (uid 1) share prefixes, and each gets only
    the "other" bits of the other's files. Wine used to map installer DACLs to
    0770 (payload) or 0000 (title) modes. This sets directories to 0777 and
    files to 0666, or 0777 if any execute bit is set, through the FTP server
    (which may chmod anything), and records every change. A directory is
    changed before it is listed, so a 0000 directory is walked too."""
    root = args.folder.rstrip("/")
    if not root.startswith(f"/data/homebrew/{TITLE_ID}/") or "/../" in root + "/":
        raise SystemExit(f"prefix-permissions stays inside /data/homebrew/{TITLE_ID}: {root}")
    ftp = connect()
    changed, unlisted, seen, stack = [], [], 0, [root]
    while stack:
        directory = stack.pop()
        lines = []
        try:
            ftp.retrlines(f"LIST {directory}", lines.append)
        except error_perm as error:
            unlisted.append({"path": directory, "reply": str(error)})
            continue
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) < 9 or parts[8] in (".", "..") or parts[0][0] not in "d-":
                continue
            path, flags = f"{directory}/{parts[8]}", parts[0]
            mode = sum(1 << (8 - i) for i, c in enumerate(flags[1:10]) if c != "-")
            seen += 1
            if flags[0] == "d":
                stack.append(path)
                wanted = 0o777
            else:
                wanted = 0o777 if mode & 0o111 else 0o666
            if mode != wanted:
                reply = ftp.sendcmd(f"SITE CHMOD {wanted:o} {path}")
                changed.append({"path": path, "old": f"{mode:o}", "new": f"{wanted:o}", "reply": reply[:3]})
    ftp.quit()
    record = {"host": HOST, "root": root, "seen": seen, "changed": changed, "unlisted": unlisted}
    Path(args.log).write_text(json.dumps(record, indent=1) + "\n")
    failed = [item for item in changed if item["reply"] != "200"]
    print(f"prefix-permissions: {root} seen={seen} changed={len(changed)} failed={len(failed)} "
          f"unlisted={len(unlisted)} log={args.log}")
    if failed or unlisted:
        raise SystemExit(1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("upload-title", help="upload ps5/title/build.sh's OUT_DIR beside wine/")
    t.add_argument("folder")
    c = sub.add_parser("title", help=f"start, close or query {TITLE_ID} through ps5vkctl")
    c.add_argument("action", choices=["launch", "kill", "restart"])
    b = sub.add_parser("title-backup", help="make a fresh raw guarded title backup")
    b.add_argument("run", help="run name; creates ps5/build/runs/RUN-backup")
    b.add_argument("--paths-from", required=True, help="prior before.json defining guarded paths")
    z = sub.add_parser("title-restore", help="restore a title-backup byte-exactly")
    z.add_argument("run", help="run name used by title-backup")
    tr = sub.add_parser("title-root-upload", help="byte-verified upload of the qualified separate B1 Wine tree")
    tr.add_argument("--folder", default=str(HERE / "build/title-psmkit-wine-b1"))
    tr.add_argument("--replace", action="store_true", help="deliberately replace an existing wine-title root only")
    b1 = sub.add_parser("b1-title-run", help="one bounded existing-prefix B1 title launch with guarded restoration")
    b1.add_argument("run", help="unique run name")
    b1.add_argument("--folder", default=str(HERE / "build/title-psmkit-wine-b1"))
    b1.add_argument("--timeout", type=float, default=240)
    b1.add_argument("--exit-grace", type=float, default=5)
    b1.add_argument("--echo", default="b1 |p1c |err:|fixme:|wine: title")
    b1.add_argument("--until", help="also end the run at this klog regex (the parent's result always ends it)")
    bi = sub.add_parser("b1-title-install",
                        help="make the qualified B1 title the home-screen program (Battle.net in the title)")
    bi.add_argument("run", help="unique install name; title-restore RUN reverts it")
    bi.add_argument("--folder", default=str(HERE / "build/title-psmkit-wine-b1"))
    pp = sub.add_parser("prefix-permissions",
                        help="open a prefix tree to payload and title alike (dirs 0777, files 0666/0777)")
    pp.add_argument("--folder", default=REMOTE + "/prefix")
    pp.add_argument("--log", required=True, help="local JSON record of every change")
    a = p.parse_args()
    {"upload-title": upload_title, "title": title,
     "title-backup": title_backup, "title-restore": title_restore,
     "title-root-upload": title_root_upload,
     "b1-title-run": b1_title_run, "b1-title-install": b1_title_install,
     "prefix-permissions": prefix_permissions}[a.cmd](a)


if __name__ == "__main__":
    main()
