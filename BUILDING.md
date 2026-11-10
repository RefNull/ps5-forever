# Building Forever Launcher on Linux

Use an x86-64 Linux host with Docker and a fresh, writable source directory.
No console, shared SDK, remote RADV build, prefix, installed game, or Mac build
output is used. The only network inputs during the build are pinned tool and
source downloads; source repositories are supplied by the caller.

## Source layout and command

`deps.json` records each external checkout's GitHub URL, commit, and path
relative to `SOURCE_ROOT`, including the required nested graphics submodules.
After checking out this repository at `SOURCE_ROOT/ps5-proton/ps5-wow`, run
`python3 ps5/release/fetch-sources.py SOURCE_ROOT` from that checkout. It fetches
the exact public commits, including nested dependencies, without recursively
fetching Proton's unused desktop submodules:

```text
SOURCE_ROOT/
  PS5_Vulkan/
  PS5_Mesa/
  PS5_PayloadSDK/
  pacbrew-repo/
  ps5-proton/
    ps5-wine/
    ps5-wow/                 # this release's source
      vkd3d-proton/          # nested checkouts listed in deps.json
      dxvk/
```

`sdk_revision` and `radv_revision` select the source objects exported for the
actual Mac SDK and patched title RADV. `PS5_MESA_REVISION` can override the latter.
These objects must exist locally; no build stage fetches private ancestry.
Public single-commit snapshots can set these selectors to their new commit IDs.
The checkout `commit` records provenance; `content_sha256` independently pins
tracked content and permits history-free replacement commits. Its input is the
NUL-joined `git ls-files --stage -z` records, without the final NUL or mode-160000
gitlink records (each nested source has its own pin). Modified tracked sources
are refused, and the Wine lock must agree with its manifest commit.

From `ps5-wow`:

```sh
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.024.001
```

The tag must equal the source `sce_sys/param.json` content version. Source
validation and the full host suite run first; the suite builds and runs its
tests in parallel (`JOBS`) and ends with `PASS`/`FAIL` per test and a
`HOST_TESTS result=` line. The script then builds the Docker
image, the SDK, native title tooling/clean-room libc, patched RADV,
whisper.cpp/ggml, FreeType, GnuTLS, Wine, 64-bit vkd3d-proton and DXVK `dxgi`
(the only graphics DLLs that ship), the patched
AddonForge headless core, both B1 roles, and the release archive. Generated Wine
server headers are restored on exit. Use an empty source/output directory for a
cold build; rerunning can reuse outputs.

Outputs under `ps5-wow/ps5/build/`:

- `whisper/{include,lib}`: target speech-engine headers and five static archives.
- `title-psmkit-wine-b1/`: independently qualified maintenance and game roles.
- `release/PPSA99030.zip`: catalog archive, with size and SHA-256 in the output.
- `release/logs/`: complete per-stage logs and `timings.tsv` (seconds/status).

Require two `RUNTIME_TITLE_QUALIFIED` records with `raw_syscalls=0`, followed by
`RUNTIME_TITLE_DUAL_QUALIFIED`. The qualifier's deliberate unknown-import and
win32u-identity rejection records are expected negative tests. `inspect-runtime.py`
also checks the GPU title's `attribute3` (high-frame-rate `0x40` and VRR
`0x40000` set, `0x80000` clear). Qualification is a host build/inspection gate,
**not console execution**.

### Voice-to-Text dependencies

`ps5/build-whisper.sh` runs after SDK, native tooling and RADV, before the title
runtime build. Its source archives are SHA-256-pinned in `ps5/release/deps.json`:
whisper.cpp 1.9.5 at `d1be6fde11ac6e0407606b4e42fe72d34add8037` and ggml 0.26.0
at `d7cb574130e6f01ad25b3289685489200febcd74`. whisper's bundled ggml is not used.
The install is `ps5/build/whisper/{include,lib}` and contains `libwhisper.a`,
`libggml.a`, `libggml-base.a`, `libggml-cpu.a` and `libggml-vulkan.a`.

After the SDK and RADV outputs exist, rerun just the speech stage:

```sh
JOBS=8 bash ps5/release/build-linux.sh /absolute/SOURCE_ROOT 01.024.001 --whisper-only
```

This uses the same `build_whisper_cached` function and cache as a full release,
without host tests, prerequisites, or Wine/title outputs. It is not runtime
qualification. Inside the release container, the equivalent entry is
`bash ps5/release/build-linux.sh --container 01.024.001 --whisper-only`. Both use
`PS5_PAYLOAD_SDK=/sources/PS5_Vulkan/.deps/native/ps5-payload-sdk`.
`ps5/build-whisper.sh` can also run directly: `PS5_WHISPER_BUILD` and
`PS5_WHISPER_INSTALL` select empty work/install directories for a cold build
(defaults `ps5/build/whisper-build` and `ps5/build/whisper`). A completed install
with a matching input stamp is skipped.

The CMake toolchain targets `x86_64-sie-ps5`, libc++, PIC and emulated TLS.
CPU AVX/AVX2/FMA/F16C and Vulkan are enabled; host-native tuning, AVX-512,
OpenMP, BLAS and ccache are disabled. The container's pinned `glslc` generates
Vulkan shaders with a host shader-generator executable. Vulkan C/C++ headers
come from the pinned vkd3d-proton Vulkan-Headers checkout; a private include
shim exposes only SPIR-V headers, not host libc headers. The recipe keeps the
SDK `isascii` compatibility header, no-host-libm override, `GGML_MAX_NAME=160`,
and 192 MiB CPU / 128 MiB Vulkan allocation-block limits. The speech model is
downloaded at runtime, never bundled into the dependency or release archives.

### AddonForge

`deps.json` pins AddonForge commit `0dc9ab28f207f86e33770e359f2345043ef19585`
and its tracked-content fingerprint. The numbered patches in
`ps5/addonforge/patches/` are applied to a copy; the upstream checkout is never
modified. CI compiles in the committed `ps5/addonforge/github-catalogue.csv`
snapshot rather than fetching it, and packages it as
`wine-title/addonforge/github-catalogue.csv`.

The container pins rustup-init 1.28.2 by SHA-256 and Rust 1.99.1 with the
`x86_64-pc-windows-gnullvm` target. `cargo vendor --locked --versioned-dirs`
populates the persistent cache and each vendored `.cargo-checksum.json` is
verified; later Cargo commands use `--offline --locked`. The `addonforge` cache
key covers the source pin, patches, `Cargo.lock`, Rust toolchain, container image
and catalogue snapshot. A cache miss builds twice in empty target directories and
refuses unequal `addonforge-core.exe` SHA-256 values; `build-receipt.json`
records both. Rust source paths map to `/usr/src/addonforge` and vendor-cache
paths to `/cache`, so the cache location does not change the executable.

`addonforge-core.exe` and `forever-maintenance.exe` are Windows PE executables
run by Wine, not payload ELFs, so the homebrew catalog scan accepts them like
Wine's other PEs.

### Persistent release-build cache

Linux release builds keep compiled outputs in `SOURCE_ROOT/.cache` so the
self-hosted runner can reuse them even though checkout cleans
`ps5-wow/ps5/build`. RADV, whisper.cpp/ggml, FreeType, GnuTLS, vkd3d-proton, DXVK
and AddonForge entries are keyed by their pinned sources and downloads, build
recipes, the Dockerfile (not the local image ID, so a rebuilt image or another
machine reuses entries), and relevant toolchain environment. Every restored
entry is checked against a SHA-256 manifest; a mismatch is a cache miss and
rebuilds that stage. Wine keeps its out-of-tree build and host-tool trees there
for incremental `make`, but reconfigures from an empty tree when its configure
key changes. Its compiler flags map the physical cache root back to `/sources`,
keeping the debug paths and release bytes of an ordinary `ps5/build` tree.

Set the Release workflow's `clean` dispatch input to discard this cache before
building. The workflow keeps the two newest entries per stage and logs
`CACHE_HIT`, `CACHE_MISS`, and `CACHE_SIZE` receipts. Host tests, both B1 runtime
roles, packaging, and qualification always run from fresh product outputs; the
package allowlist remains the only source of ZIP members.

`ps5/build-wine.sh configure` also keeps a per-tree `.configure-stamp.json`
for direct incremental builds. Its key covers the configure command,
configure/configure.ac bytes, compiler/tool paths and configure environment
(including target/repro flags, PE linker flags and library sonames). Matching
inputs skip configure, not `make`. Changed C/C++ or PE compiler flags clean the
affected build tree before configuring, because Wine does not track flag changes
in object dependencies. A tree without a stamp is configured; its existing
Makefile flags are compared first so switching flags cannot reuse old objects.
Generated Makefile/config.status/config.h hashes also invalidate the stamp if
another build reconfigures the shared tree without this helper.

Runtime construction decodes each compile-database command and resolves its
output once per role. The qualifier binds its own source hash into each
qualification key, so even a behaviour-preserving qualifier edit changes the
required receipts; no receipt or file-identity check is reused or skipped.

The package writer compresses independent members with up to 24 threads
(`package.py --jobs 1` selects one). It keeps the serial writer's local headers,
compression settings, member order and central-directory metadata; workers spool
to temporary files and hold at most one member each.
`make -C ps5/probes/psmkit-wine test-build-speed` covers configure reuse/clean
decisions and parallel-versus-serial ZIP byte equality.

## Pinned inputs and limits

`Dockerfile` pins the Ubuntu 24.04 image by digest and Ubuntu apt packages
(including LLVM/Clang 18 and 19, SPIR-V translator/libclc 19, glslang, glslc,
SPIR-V headers, Python, bison, CMake and autotools) by the immutable 2026-10-01
archive snapshot. LLVM 19 builds native/title code and RADV's LLVM-dependent host
tools. llvm-mingw 20260922/UCRT (LLVM 23.1.2) and rustup-init 1.28.2 are SHA-256
checked; Rust 1.99.1 and the `x86_64-pc-windows-gnullvm` target are exact
toolchain pins. Python build packages have explicit versions. Ubuntu's fixed
`ca-certificates=20240203` package is unpacked only to bootstrap HTTPS trust;
nothing is installed from the live Ubuntu archive before switching to the snapshot.

The release container sets `SOURCE_DATE_EPOCH=0` and passes
`PS5_PE_LDFLAGS=-Wl,--no-insert-timestamp` to Wine and both graphics builders.
This requests zero timestamps from the PE linker; Wine-generated export and
data-only headers instead keep deterministic filename/CPU hashes. Mac builds keep
their flags unless that variable is set. These settings alone do not prove
byte-for-byte release reproducibility.

The Linux image replaces llvm-mingw's i686/x86_64 `as` entrypoints with
assemble-only compiler invocations (`-x assembler -c`). The upstream entrypoints
alias the generic Clang wrapper, which attempts an executable link when Wine
assembles individual import-library members and then reports missing `WinMain`
and `__wine_import_*_name` symbols. This adapter does not choose PE section
permissions.

Wine's i386/x86_64 delay-import emitter (`tools/winebuild/import.c`) declares its
`.text$1` thunks as COFF code with read/execute (`"xr"`) attributes. Without it,
LLVM's assembler treats the subsection as writable data and LLD emits a second,
non-executable `.text` that faults at the first delayed call. After changing
winebuild, rebuild the Wine host `tools/winebuild/winebuild`, regenerate both
architectures' `*.delay.a` libraries and relink their PE consumers before
restaging the runtime.

`deps.json` inventories SHA-256-pinned downloads. The recipes also verify those
digests: FreeType; pacbrew Nettle/mini-gmp, libtasn1 and GnuTLS; Vulkan native
zlib; Mesa's zlib and Meson patch; Mozilla roots; Droid/Noto fonts and notices.
Repository-contained assets, headers and stb_image come from the release source.
Optional workstation desktop artwork is disabled. The menu's glyph tables
(`ps5/title/menu-font.h`) are committed so builds stay network-free;
`ps5/title/tools/make-menu-font.py` regenerates them from SHA-256-pinned Noto
Sans and Noto Sans Symbols 2 files.

Wine and the graphics DLLs normally stamp `git describe`, which depends on
unpublished tags/history and can silently fall back in shallow snapshots.
After validating source fingerprints, the build generates a narrow Git wrapper
that replays only the three known graphics version-stamp commands from explicit
`describe`/`original_commit` provenance. All other Git commands use `/usr/bin/git`.
Wine's ntdll string is the fork's annotated release tag on the pinned commit
(`ps5-wine` `describe`, checked by `test-pins`); `build-wine.sh` takes it as
`WINE_DESCRIBE` and fails if `dlls/ntdll/unix/version.c` differs.
Generated headers in the build trees show these values.

The SDK's `platform/tools/setup-sdk.sh` rebuilds the fork's headers, platform,
libc, CRT and additional import stubs over the hash-pinned upstream **v0.42
binary SDK**; its libc++, libc++abi, libunwind and existing stubs are not rebuilt
from source. The Docker image likewise consumes published compiler/package
binaries. This is a pinned-source/pinned-binary bootstrap, not a fully
bootstrapped source-only toolchain. No proprietary or workstation-only binary is
supplied, and no private repository or development history is needed.

A cold build needs about **30 GiB free**; once sources and the image exist,
`build-linux.sh` requires 20 GiB if any stage lacks a cache entry for this
build's key and 10 GiB if none does. CI targets a self-hosted Linux
x86-64 runner and defaults to the container's `nproc` value (override with the
`FOREVER_JOBS` repository variable). RADV Ninja and Wine's `install-lib` honour
the same cap. The container runs with the invoking user's UID/GID, writes only
its temporary home and the supplied source/build tree, and never removes host
toolchains.

## Host checks

After the pinned build image exists, run the host suite from a source tree with
dependencies fetched. It keeps compiler warnings as errors and does not contact
the console. These checks are not native PS5 execution.

```sh
SOURCE_ROOT=/absolute/SOURCE_ROOT
docker run --rm --pull=never --user "$(id -u):$(id -g)" \
  --env HOME=/tmp/forever-host-tests \
  --mount "type=bind,source=$SOURCE_ROOT,target=/sources" \
  --workdir /sources/ps5-proton/ps5-wow \
  forever-launcher-linux:ubuntu-24.04 \
  bash -c 'mkdir -p "$HOME"; make -s -j"$(nproc)" -O -k -C ps5/probes/psmkit-wine test MINGW_CC=/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang'
```

`make -C ps5/probes/psmkit-wine list-host-tests` names every test that `test`
runs; each one is also a `run-<name>` target.

Several gates (`test-write-buffer`, `test-file-security`, `test-cursor`,
`test-direct-*`) compile production functions extracted from `WINE_SRC`, so use
the Wine revision pinned for this source, not an older release.

After building Wine's host tools, run the delay-import regression inside the
same image (paths relative to `ps5-wow`):

```sh
make -s -C ps5/probes/psmkit-wine test-winebuild-delayload \
  LLVM_MINGW=/opt/llvm-mingw \
  WINEBUILD=../../build/wine-ps5-tools/tools/winebuild/winebuild
```

It links i386 and x86_64 PE consumers against Wine's real import archives and
MinGW delay-load helper and follows each initialized IAT pointer to its
first-call thunk. Thunks must be read/execute without write; the IAT must be
writable without execute. No Wine runtime, title build or console is needed.

For the macOS compatibility checks (not a full title rebuild):

```sh
PATH=/tmp/py314:$PATH make -s -C ps5/probes/psmkit-wine test
bash -n ps5/release/build-linux.sh # repeat for edited build/staging scripts
```

The portable scripts keep the Mac SDK, Homebrew and llvm-mingw defaults; Linux
selects its tool paths through the container environment.

`python3 ps5/writebroker/benchmark-writebroker.py --output /tmp/wb-native
--sdk /path/to/payload-sdk --wine /path/to/ps5-wine` prepares native and broker
benchmark ELFs without contacting a console; see its header for console modes.

## Diagnostic builds and tuning

- Profiling: copy `b1-product.json`, set `environment.WINEPS5_PROFILE="1"` and
  build the maintenance role with that `B1_PROFILE_CONFIG`. `runtime-b1-dual`
  selects its own product profile, so compose separately built roles when using a
  diagnostic profile. `GetThreadTimes` is not implemented on the PS5 platform;
  use the sampler's `title vm cpu`, `title request latency` and `server queue`
  lines.
- Side-by-side test app: set `FOREVER_TITLE_ID` (and optionally
  `FOREVER_TITLE_NAME`) for `runtime-b1-dual` and `package.py`. Only the app's
  identity changes: the Wine build (`ps5/paths.sh`) and the user data root
  `/data/forever` are shared, and such an app does not set the home-screen
  icon.
- Write-buffer decisions: build Wine with
  `WINE_PS5_EXTRA_CFLAGS=-DWINE_PS5_WRITE_BUFFER_DIAG=1`.
- Live switches in `C:\wineps5-tune.txt`: `write_buffer=off` (or
  `WINEPS5_WRITE_BUFFER=0`) restores synchronous WoW archive writes; `direct_commit_batch=off` restores per-unit direct-commit
  protection; `direct_cache=off` disables the `Agent.exe` direct-region cache.
  All default to on.

## Native release gate

Before publishing a release or promoting its catalog entry, install the exact
final `PPSA99030.zip` on the PS5. Repeat the raw drive probe in both 32-bit and
64-bit modes, then run Wine's opt-in, read-only regression
`kernel32_test.exe volume ps5-title-drives` in both architectures. Require only
`C:`/`W:` in drive enumeration, local fixed-disk device information for both
roots and their DOS-device targets, and rejection of unsupported `D:`, `Z:` and
UNC paths. Finally restore the default Battle.net startup and confirm its install
destination is accepted rather than rejected as a network folder. The
delay-import regression gate above is also required.
