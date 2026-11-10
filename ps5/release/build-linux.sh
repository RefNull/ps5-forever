#!/usr/bin/env bash
# Build in the pinned Ubuntu container; never uses the remote RADV build tree.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Every pin is read from deps.json; see pins.py.
pin() { python3 "$here/pins.py" "$@"; }

if [[ ${1:-} != --container ]]; then
    [[ $# == 2 || ( $# == 3 && $3 == --whisper-only ) ]] || {
        echo "usage: $0 SOURCE_ROOT NN.NNN.NNN [--whisper-only]" >&2; exit 2;
    }
    sources=$(cd -- "$1" && pwd)
    tag=$2
    image=forever-launcher-linux:ubuntu-24.04
    start=$SECONDS
    # Cache keys hash the Dockerfile as the image's whole definition (base
    # digest, apt snapshot, checksummed toolchains), so it must not read files
    # from the build context.
    if grep -Eiq '^[[:space:]]*(COPY|ADD)[[:space:]]' "$here/Dockerfile"; then
        echo 'Dockerfile must not COPY or ADD: the cache keys hash only the Dockerfile' >&2; exit 2;
    fi
    docker build --platform linux/amd64 --tag "$image" "$here"
    printf 'BUILD_STAGE image seconds=%s\n' "$((SECONDS - start))"
    exec docker run --rm --platform linux/amd64 \
        --user "$(id -u):$(id -g)" \
        --env HOME=/tmp/forever-builder \
        --mount "type=bind,source=$sources,target=/sources" \
        --env "JOBS=${JOBS:-}" \
        --env "FOREVER_BUILD_CACHE=/sources/.cache" \
        --env "FOREVER_CLEAN_CACHE=${FOREVER_CLEAN_CACHE:-0}" \
        --env "FOREVER_TITLE_ID=${FOREVER_TITLE_ID:-}" \
        --env "FOREVER_TITLE_NAME=${FOREVER_TITLE_NAME:-}" \
        --env "PS5_MESA_REVISION=${PS5_MESA_REVISION:-}" "$image" \
        bash /sources/ps5-proton/ps5-wow/ps5/release/build-linux.sh --container "$tag" "${@:3}"
fi
[[ ( $# == 2 || ( $# == 3 && $3 == --whisper-only ) ) &&
   $(uname -s) == Linux && $(uname -m) == x86_64 ]] || {
    echo 'the container entry requires Linux x86_64, a content-version tag and optional --whisper-only' >&2; exit 2;
}
tag=$2
root=/sources
wow=$root/ps5-proton/ps5-wow
cd "$wow"
if [[ -z ${JOBS:-} ]]; then
    JOBS=$(nproc)
fi
export JOBS
export BUILD_JOBS=$JOBS
export CMAKE_BUILD_PARALLEL_LEVEL=$JOBS
export PS5_PE_LDFLAGS=-Wl,--no-insert-timestamp
export PS5_VULKAN=$root/PS5_Vulkan
export PS5_MESA_FORK=$root/PS5_Mesa
export PACBREW_REPO=$root/pacbrew-repo
export WINE_SRC=$root/ps5-proton/ps5-wine
export PS5_PAYLOAD_SDK=$PS5_VULKAN/.deps/native/ps5-payload-sdk
export PATH=$LLVM_MINGW/bin:$PATH
export PS5_RADV_REVISION=${PS5_MESA_REVISION:-$(pin value radv_revision)}
export FOREVER_BUILD_CACHE=${FOREVER_BUILD_CACHE:-$root/.cache}
if [[ ${FOREVER_CLEAN_CACHE:-0} == 1 ]]; then
    rm -rf -- "$FOREVER_BUILD_CACHE"
    printf 'CACHE_CLEAN root=%s\n' "$FOREVER_BUILD_CACHE"
fi
mkdir -p "$FOREVER_BUILD_CACHE" "$HOME"
# The writebroker's existing Makefile uses this historic path.
ln -s "$PS5_PAYLOAD_SDK" "$HOME/ps5-payload-sdk-platform"
# The desktop picture is the committed, drawn sce_sys/pic0.dds; never import
# optional, untracked artwork from a workstation.
unset DESKTOP_DDS
mkdir -p ps5/build/release/logs
logs=$wow/ps5/build/release/logs
cache_helper=$here/build-cache.py
export FOREVER_SOURCE_ROOT=$root
export FOREVER_SOURCE_MANIFEST=$here/deps.json
export PATH=$logs/tools:$PATH
printf 'stage\tseconds\tstatus\n' > "$logs/timings.tsv"
started=$SECONDS
stage() {
    local name=$1 begin=$SECONDS status=0
    local -a statuses
    shift
    printf '\n==> BUILD_STAGE_BEGIN %s %s\n' "$name" "$(date -u +%FT%TZ)"
    # Do not put a build function in an `||` condition: Bash would disable
    # errexit inside it and a failed prerequisite could be silently skipped.
    set +e
    (set -e; "$@") 2>&1 | tee "$logs/$name.log"
    statuses=("${PIPESTATUS[@]}")
    set -e
    status=${statuses[0]}
    (( status != 0 )) || status=${statuses[1]}
    # Preserve failure status even if printing the receipt itself succeeds.
    printf '%s\t%s\t%s\n' "$name" "$((SECONDS - begin))" "$status" >> "$logs/timings.tsv"
    printf 'BUILD_STAGE %s seconds=%s status=%s\n' "$name" "$((SECONDS - begin))" "$status"
    return "$status"
}
validate_sources() {
    pin validate-sources "$root" "$tag" "$PS5_RADV_REVISION"
    # Replaying explicit version provenance avoids dependence on unpublished
    # Git ancestry. Only these three graphics version-stamp commands change;
    # all Git source/export/validation operations use the real executable.
    # Wine's stamp goes to build-wine.sh as WINE_DESCRIBE instead: that script
    # puts /usr/bin (BISON_BIN) ahead of this wrapper on PATH.
    mkdir -p "$logs/tools"
    cat > "$logs/tools/git" <<'PY'
#!/opt/venv/bin/python3
import json
import os
from pathlib import Path
import subprocess
import sys

real_git = '/usr/bin/git'
args = sys.argv[1:]
known = {
    ('describe', '--always', '--tags', '--dirty=+'): 'vkd3d-proton',
    ('describe', '--always', '--exclude=*', '--abbrev=15', '--dirty=0'): 'vkd3d-proton',
    ('describe', '--dirty=+'): 'dxvk',
}
name = known.get(tuple(args))
if name:
    manifest = json.loads(Path(os.environ['FOREVER_SOURCE_MANIFEST']).read_text())
    source = next(item for item in manifest['sources'] if item['name'] == name)
    expected = (Path(os.environ['FOREVER_SOURCE_ROOT']) / source['path']).resolve()
    if os.environ.get('GIT_DIR'):
        git_dir = Path(os.environ['GIT_DIR']).resolve()
        actual = git_dir.parent if git_dir.name == '.git' else None
    else:
        result = subprocess.run([real_git, 'rev-parse', '--show-toplevel'],
                                capture_output=True, text=True)
        actual = Path(result.stdout.strip()).resolve() if result.returncode == 0 else None
    if actual == expected:
        value = (source['original_commit'][:15] if '--abbrev=15' in args else source['describe'])
        print(value)
        raise SystemExit(0)
os.execv(real_git, [real_git, *args])
PY
    chmod +x "$logs/tools/git"
}
build_sdk() {
    local revision tree=$root/.sdk-source
    revision=$(pin value sdk_revision)
    mkdir -p "$tree"
    git -C "$root/PS5_PayloadSDK" archive "$revision" | tar -x -C "$tree"
    bash "$tree/platform/tools/setup-sdk.sh" \
        "$PS5_PAYLOAD_SDK" "$revision" "$root/.downloads"
}
build_host_tools() {
    bash "$PS5_VULKAN/tools/setup-native-dependencies.sh" --skip-sdk
    local native=$PS5_VULKAN/tooling/native
    local zlib=$PS5_VULKAN/.deps/native/zlib/root
    mkdir -p "$PS5_VULKAN/build/host/clang-resource/lib/linux"
    clang++-19 -std=c++20 -O2 -Wall -Wextra -Werror -I "$zlib/usr/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$zlib/usr/lib/libz.a" -o "$PS5_VULKAN/build/host/ps5-native-tool"
    cp "$(clang-19 -print-file-name=libclang_rt.builtins-x86_64.a)" \
        "$PS5_VULKAN/build/host/clang-resource/lib/linux/"
    bash "$PS5_VULKAN/tools/rebuild-libc.sh"
}
restore_wine_headers() {
    git -C "$WINE_SRC" restore include/wine/server_protocol.h \
        server/request_handlers.h server/request_trace.h
}
cache_key_common() {
    local stage=$1
    shift
    python3 "$cache_helper" key --stage "$stage" --source-root "$root" \
        --manifest "$here/deps.json" \
        --path "$cache_helper" \
        --path "$here/Dockerfile" \
        --value "LLVM_MINGW=$LLVM_MINGW" \
        --value "PS5_PAYLOAD_SDK=$PS5_PAYLOAD_SDK" \
        --value "PS5_VULKAN=$PS5_VULKAN" \
        --value "PS5_PE_LDFLAGS=$PS5_PE_LDFLAGS" "$@"
}
cache_key_radv() {
    cache_key_common radv \
        --source PS5_Mesa --source PS5_Vulkan --source PS5_PayloadSDK \
        --download "$(pin download radv-zlib name)" --download radv-zlib-meson-patch \
        --path "$wow/ps5/radv" \
        --value "PS5_RADV_REVISION=$PS5_RADV_REVISION" \
        --value "PS5_MESA_FORK=$PS5_MESA_FORK" \
        --value "LLVM_CONFIG=${LLVM_CONFIG:-}"
}
# whisper names RADV's archive only as CMake's Vulkan_LIBRARY: it installs
# static archives and headers that contain no RADV bytes or path, and the
# title links RADV itself. So RADV is not part of whisper's key.
cache_key_whisper() {
    cache_key_common whisper \
        --source PS5_PayloadSDK \
        --source vkd3d-proton/khronos/Vulkan-Headers \
        --download "$(pin download whisper.cpp name)" --download "$(pin download ggml name)" \
        --path "$wow/ps5/build-whisper.sh" \
        --path "$wow/ps5/whisper" \
        --path "$wow/ps5/build-common.sh"
}
cache_key_freetype() {
    cache_key_common freetype \
        --source PS5_PayloadSDK --download "$(pin download freetype name)" \
        --path "$wow/ps5/build-freetype.sh" \
        --path "$wow/ps5/build-common.sh" \
        --path "$wow/ps5/paths.sh"
}
cache_key_gnutls() {
    cache_key_common gnutls \
        --source PS5_PayloadSDK --source pacbrew-repo \
        --download "$(pin download nettle name)" --download "$(pin download libtasn1 name)" \
        --download "$(pin download gnutls name)" \
        --path "$wow/ps5/build-gnutls.sh" \
        --path "$wow/ps5/build-common.sh" \
        --path "$wow/ps5/paths.sh" \
        --value "PACBREW_REPO=$PACBREW_REPO"
}
# Release builds only what ships: 64-bit d3d12/d3d12core and 64-bit dxgi
# (runtime-files.txt share/forever/system32; build-runtime.py --*-dlls).
export VKD3D_ARCHS=64 DXVK_ARCHS=64 DXVK_DLLS=dxgi
cache_key_vkd3d() {
    cache_key_common vkd3d-proton \
        --source-prefix vkd3d-proton \
        --path "$wow/ps5/build-vkd3d-proton.sh" \
        --path "$wow/ps5/vkd3d-proton" \
        --value "VKD3D_ARCHS=$VKD3D_ARCHS" \
        --value "PS5_CPU_FLAGS=${PS5_CPU_FLAGS:-}" \
        --value "WIDL=${WIDL:-}"
}
cache_key_dxvk() {
    cache_key_common dxvk \
        --source-prefix dxvk \
        --path "$wow/ps5/build-dxvk.sh" \
        --value "DXVK_ARCHS=$DXVK_ARCHS" \
        --value "DXVK_DLLS=$DXVK_DLLS" \
        --value "PS5_CPU_FLAGS=${PS5_CPU_FLAGS:-}"
}
cache_key_addonforge() {
    cache_key_common addonforge \
        --source AddonForge \
        --path "$wow/ps5/addonforge" \
        --value "rust-toolchain=$(pin value rust_toolchain)" \
        --value "rust-target=x86_64-pc-windows-gnullvm" \
        --value "github-catalogue-sha256=$(sha256sum "$logs/github-catalogue.csv" | cut -d' ' -f1)" \
        --value "rustflags=crt-static,debuginfo-0,no-insert-timestamp,remap-paths"
}
# Wine links the FreeType and GnuTLS outputs. It reads nothing from
# vkd3d-proton, which builds after it, so vkd3d is not part of this key.
cache_key_wine() {
    local sdk_revision freetype_key gnutls_key
    sdk_revision=$(pin value sdk_revision)
    freetype_key=$(cache_key_freetype)
    gnutls_key=$(cache_key_gnutls)
    cache_key_common wine \
        --source PS5_PayloadSDK --source PS5_Vulkan \
        --path "$wow/ps5/build-wine.sh" \
        --path "$wow/ps5/wine" \
        --path "$wow/ps5/paths.sh" \
        --path "$WINE_SRC/configure.ac" \
        --path "$WINE_SRC/configure" \
        --value "sdk-revision=$sdk_revision" \
        --value "freetype-output-key=$freetype_key" \
        --value "gnutls-output-key=$gnutls_key" \
        --value "PS5_CPU_FLAGS=${PS5_CPU_FLAGS:-}" \
        --value "CROSSLDFLAGS=${CROSSLDFLAGS:-}" \
        --value "WINE_REPRO_FLAGS=-fdebug-compilation-dir=<logical-wine-build-path>" \
        --value "WINE_TOOLS_REPRO_FLAGS=-ffile-prefix-map=<cache-output-root>=$root" \
        --value "HOST_FREETYPE=${HOST_FREETYPE:-/opt/homebrew/opt/freetype}"
}
cached_build() {
    local name=$1 key=$2
    local -a outputs=() helper_args=()
    shift 2
    while [[ $1 != -- ]]; do
        outputs+=("$1")
        shift
    done
    shift
    for output in "${outputs[@]}"; do
        helper_args+=(--output "$output")
    done
    if python3 "$cache_helper" restore --cache "$FOREVER_BUILD_CACHE" \
        --stage "$name" --key "$key" --source-root "$root" "${helper_args[@]}"; then
        printf 'CACHE_HIT %s key=%s\n' "$name" "${key:0:12}"
        return
    fi
    printf 'CACHE_MISS %s key=%s\n' "$name" "${key:0:12}"
    python3 "$cache_helper" clear --source-root "$root" "${helper_args[@]}"
    "$@"
    python3 "$cache_helper" store --cache "$FOREVER_BUILD_CACHE" \
        --stage "$name" --key "$key" --source-root "$root" "${helper_args[@]}"
    python3 "$cache_helper" prune --cache "$FOREVER_BUILD_CACHE" --keep 2
}
build_radv_cached() {
    local key
    key=$(cache_key_radv)
    cached_build radv "$key" \
        ps5-proton/ps5-wow/ps5/build/radv-release-patched -- \
        python3 ps5/radv/build-radv.py
}
build_whisper_cached() {
    local key
    key=$(cache_key_whisper)
    cached_build whisper "$key" \
        ps5-proton/ps5-wow/ps5/build/whisper -- \
        bash ps5/build-whisper.sh
}
build_freetype_cached() {
    local key
    key=$(cache_key_freetype)
    cached_build freetype "$key" \
        ps5-proton/ps5-wow/ps5/build/freetype-ps5 \
        ps5-proton/ps5-wow/ps5/build/freetype-ps5-build -- \
        bash ps5/build-freetype.sh
}
build_gnutls_cached() {
    local key
    key=$(cache_key_gnutls)
    cached_build gnutls "$key" \
        ps5-proton/ps5-wow/ps5/build/gnutls-ps5 \
        ps5-proton/ps5-wow/ps5/build/gnutls-stage -- \
        bash ps5/build-gnutls.sh
}
build_wine() {
    local key hit=false wine_build_real cache_outputs
    local build=ps5-proton/ps5-wow/ps5/build
    key=$(cache_key_wine)
    # Patch the checkout before the source index and server headers are read.
    bash ps5/build-wine.sh patch
    WINE_DESCRIBE=$(pin source ps5-wine describe)
    export WINE_DESCRIBE
    if python3 "$cache_helper" wine-prepare --cache "$FOREVER_BUILD_CACHE" \
        --key "$key" --source-root "$root" \
        --directory "$build/wine-ps5-build" \
        --directory "$build/wine-ps5-tools" \
        --directory "$build/ps5-lib" \
        --file "$build/ps5_paths.h"; then
        hit=true
        printf 'CACHE_HIT wine key=%s\n' "${key:0:12}"
    else
        printf 'CACHE_MISS wine key=%s\n' "${key:0:12}"
    fi
    wine_build_real=$(realpath "$root/$build/wine-ps5-build")
    cache_outputs=${wine_build_real%/$build/wine-ps5-build}
    [[ $cache_outputs != "$wine_build_real" && -d $cache_outputs ]] || {
        printf 'cannot resolve Wine cache output root from %s\n' "$wine_build_real" >&2
        return 1
    }
    # MinGW's -ffile-prefix-map also changes CodeView source separators. Set
    # only its compilation directory; source paths already live below /sources.
    # Native host tools are not shipped, so their GCC-compatible prefix map can
    # cover the physical cache directory as a whole.
    export WINE_REPRO_FLAGS="-fdebug-compilation-dir=$root/$build/wine-ps5-build"
    export WINE_TOOLS_REPRO_FLAGS="-ffile-prefix-map=$cache_outputs=$root"
    python3 "$cache_helper" wine-sync-source --cache "$FOREVER_BUILD_CACHE" \
        --key "$key" --source "$WINE_SRC"
    python3 "$cache_helper" wine-restore-headers --cache "$FOREVER_BUILD_CACHE" \
        --key "$key" --source "$WINE_SRC" || true
    (cd "$WINE_SRC" && perl tools/make_requests)
    python3 "$cache_helper" wine-save-headers --cache "$FOREVER_BUILD_CACHE" \
        --key "$key" --source "$WINE_SRC"
    if [[ $hit == false ]]; then
        bash ps5/build-wine.sh configure
        python3 "$cache_helper" wine-mark-configured \
            --cache "$FOREVER_BUILD_CACHE" --key "$key"
    fi
    bash ps5/build-wine.sh all
    python3 "$cache_helper" wine-commit --cache "$FOREVER_BUILD_CACHE" \
        --key "$key" --source "$WINE_SRC"
    python3 "$cache_helper" prune --cache "$FOREVER_BUILD_CACHE" --keep 2
}
build_vkd3d_cached() {
    local key
    key=$(cache_key_vkd3d)
    cached_build vkd3d-proton "$key" \
        ps5-proton/ps5-wow/ps5/build/vkd3d-proton-64/dlls -- \
        bash ps5/build-vkd3d-proton.sh
}
build_dxvk_cached() {
    local key
    key=$(cache_key_dxvk)
    cached_build dxvk "$key" \
        ps5-proton/ps5-wow/ps5/build/dxvk-64/dlls -- \
        bash ps5/build-dxvk.sh
}
build_addonforge_cached() {
    local key
    cp "$wow/ps5/addonforge/github-catalogue.csv" "$logs/github-catalogue.csv"
    key=$(cache_key_addonforge)
    cached_build addonforge "$key" \
        ps5-proton/ps5-wow/ps5/build/addonforge -- \
        python3 ps5/addonforge/build.py \
            --source "$root/AddonForge" \
            --output "$wow/ps5/build/addonforge" \
            --cache "$FOREVER_BUILD_CACHE" \
            --catalogue-snapshot "$logs/github-catalogue.csv"
}
# Free space for what this run still writes; the image and sources already
# exist. A measured cold run grew the cache by 4.8 GiB and the build tree by
# 7.2 GiB; cache hits are symlinks, so a warm run writes at most that tree.
check_disk() {
    local name key need free
    local -a missing=()
    cp "$wow/ps5/addonforge/github-catalogue.csv" "$logs/github-catalogue.csv"
    for name in radv whisper freetype gnutls vkd3d dxvk addonforge wine; do
        key=$("cache_key_$name")
        [[ $name != vkd3d ]] || name=vkd3d-proton
        python3 "$cache_helper" present --cache "$FOREVER_BUILD_CACHE" \
            --stage "$name" --key "$key" || missing+=("$name")
    done
    need=$(( ${#missing[@]} ? 20 : 10 ))
    free=$(( $(df -Pk "$root" | awk 'NR == 2 { print $4 }') >> 20 ))
    printf 'DISK free=%sGiB need=%sGiB cold=%s\n' "$free" "$need" "${missing[*]:-none}"
    (( free >= need )) || { echo "release build requires $need GiB free" >&2; return 1; }
}
cleanup() {
    local status=$?
    restore_wine_headers || status=$?
    rm -rf "$logs/tools"
    exit "$status"
}
# Dependency-only rebuilds reuse the release stage/cache implementation without
# touching Wine, title outputs or their qualification. SDK and RADV must exist.
if [[ ${3:-} == --whisper-only ]]; then
    stage whisper build_whisper_cached
    python3 "$cache_helper" size --cache "$FOREVER_BUILD_CACHE"
    printf 'BUILD_COMPLETE seconds=%s stage=whisper prefix=%s\n' \
        "$((SECONDS - started))" "$wow/ps5/build/whisper"
    exit 0
fi
trap cleanup EXIT
stage inputs validate_sources
stage disk check_disk
stage host-tests make -s -C ps5/probes/psmkit-wine test \
    HOST_CC=clang-19 \
    MINGW_CC="$LLVM_MINGW/bin/x86_64-w64-mingw32-clang"
stage sdk build_sdk
stage host-tools build_host_tools
stage radv build_radv_cached
stage whisper build_whisper_cached
stage freetype build_freetype_cached
stage gnutls build_gnutls_cached
stage wine build_wine
stage winebuild-delayload make -s -C ps5/probes/psmkit-wine test-winebuild-delayload \
    WINEBUILD="$wow/ps5/build/wine-ps5-tools/tools/winebuild/winebuild" \
    LLVM_MINGW="$LLVM_MINGW"
stage vkd3d-proton build_vkd3d_cached
stage dxvk build_dxvk_cached
stage addonforge build_addonforge_cached
stage runtime-b1-dual make -s -C ps5/probes/psmkit-wine runtime-b1-dual
stage package python3 ps5/release/package.py --tag "$tag"
python3 "$cache_helper" prune --cache "$FOREVER_BUILD_CACHE" --keep 2
python3 "$cache_helper" size --cache "$FOREVER_BUILD_CACHE"
printf 'BUILD_COMPLETE seconds=%s zip=%s\n' "$((SECONDS - started))" "$wow/ps5/build/release/${FOREVER_TITLE_ID:-PPSA99030}.zip"
