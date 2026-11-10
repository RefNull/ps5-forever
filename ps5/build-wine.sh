#!/usr/bin/env bash
# M2: build the dedicated sibling Wine PS5 fork for the PS5 on macOS or Linux.
# Unix side uses ~/ps5-payload-sdk-platform's prospero-clang (target
# x86_64-sie-ps5, which defines __FreeBSD__ and __PROSPERO__); PE side for i386
# and x86_64 with llvm-mingw; host tools (winebuild, widl, wrc, winegcc) built
# natively first.
#
#   ps5/build-wine.sh patch       apply ps5/wine/patches to the checkout, then stop
#   ps5/build-wine.sh configure   generate wine/configure if needed, configure both trees
#   ps5/build-wine.sh [targets]   make in the PS5 tree (default: loader, server, ntdll)
#
# Run ps5/build-freetype.sh and ps5/build-gnutls.sh first; host sfnt2fon uses
# Homebrew FreeType.
# JOBS (default 8) sets make's job count.
# PS5_CPU_FLAGS (default empty) appends target CPU flags to Unix and both PE
# architectures. Use WINE_BUILD for an isolated comparison, for example:
# WINE_BUILD="$PWD/ps5/build/wine-ps5-build-znver2" PS5_CPU_FLAGS="-march=znver2"
# WINE_DESCRIBE optionally fixes the version string Wine embeds in ntdll.
# WINE_REPRO_FLAGS and WINE_TOOLS_REPRO_FLAGS optionally normalize
# build-directory paths in debug data; release cache orchestration sets them,
# while ordinary builds leave them empty.
# WINE_CCACHE_DIR optionally caches the PE cross compilers' objects there with
# ccache (WINE_CCACHE, default `ccache` on PATH); the release build sets both.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# WINE_SRC selects another checkout when explicitly comparing revisions.
# Production defaults to the standalone Wine PS5 fork beside this repository.
src=${WINE_SRC:-$here/../../ps5-wine}
sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5-payload-sdk-platform}/bin
mingw=${LLVM_MINGW:-$HOME/llvm-mingw}
tools=${WINE_TOOLS:-$here/build/wine-ps5-tools}
cross=${WINE_BUILD:-$here/build/wine-ps5-build}
cpu_flags=${PS5_CPU_FLAGS:-}
repro_flags=${WINE_REPRO_FLAGS:-}
tools_repro_flags=${WINE_TOOLS_REPRO_FLAGS:-}
# Where ps5/stage-wine.sh installs Wine on the console (its prefix).
source "$here/paths.sh"
prefix=$WINE_ROOT
freetype=$here/build/freetype-ps5
host_freetype=${HOST_FREETYPE:-/opt/homebrew/opt/freetype}
freetype_soname=$prefix/lib/libfreetype.so
gnutls=$here/build/gnutls-ps5
gnutls_soname=$prefix/lib/libgnutls.so
vulkan_soname=$prefix/lib/libvulkan.so

# The interactive shell exports Homebrew include and library paths in CPPFLAGS
# and LDFLAGS; they must not reach a cross build.
jobs=${JOBS:-8}
wine_ps5_extra_cflags=${WINE_PS5_EXTRA_CFLAGS:-}
unset CPPFLAGS LDFLAGS CFLAGS CXXFLAGS
export PATH=$mingw/bin:${BISON_BIN:-/opt/homebrew/opt/bison/bin}:$PATH

# ccache masquerades as the PE compilers ahead of llvm-mingw on PATH, so the
# configured commands (Makefile, compile_commands.json) name the same
# compilers. A hit requires the same compiler, arguments and preprocessed
# input, so it yields the bytes the compiler would. The working directory is
# not hashed: it is the per-key cache path, and every PE compile fixes its
# debug directory with -fdebug-compilation-dir (WINE_REPRO_FLAGS). The
# directory itself must be specific to the toolchain (the caller keys it).
if [[ -n ${WINE_CCACHE_DIR:-} ]]; then
    ccache=$(command -v "${WINE_CCACHE:-ccache}")
    ccache_bin=$here/build/wine-ccache-bin
    mkdir -p "$ccache_bin" "$WINE_CCACHE_DIR"
    for name in i686-w64-mingw32-gcc i686-w64-mingw32-clang \
        x86_64-w64-mingw32-gcc x86_64-w64-mingw32-clang; do
        ln -sfn "$ccache" "$ccache_bin/$name"
    done
    export CCACHE_DIR=$WINE_CCACHE_DIR CCACHE_NOHASHDIR=1 CCACHE_COMPILERCHECK=content \
        CCACHE_MAXSIZE=${WINE_CCACHE_MAXSIZE:-4G}
    export PATH=$ccache_bin:$PATH
    "$ccache" --zero-stats > /dev/null
fi

# Wine embeds `GIT_DIR=$src/.git git describe HEAD` in ntdll (the
# dlls/ntdll/unix/version.c rule) and falls back to wine-$(PACKAGE_VERSION)
# when that fails, as it does in a depth-1 checkout without tags. WINE_DESCRIBE
# (optional) supplies the value: a git shim first on PATH answers that one
# command for this source tree and runs the real git for everything else.
version_c=$cross/dlls/ntdll/unix/version.c
if [[ -n ${WINE_DESCRIBE:-} ]]; then
    [[ $WINE_DESCRIBE =~ ^[A-Za-z0-9._+-]+$ ]] || {
        printf 'WINE_DESCRIBE is not a describe string: %s\n' "$WINE_DESCRIBE" >&2; exit 2;
    }
    real_git=$(command -v git)
    source_dir=$(cd -- "$src" && pwd -P)
    describe_bin=$(mktemp -d "${TMPDIR:-/tmp}/wine-describe.XXXXXX")
    trap 'rm -rf -- "$describe_bin"' EXIT
    cat > "$describe_bin/git" <<EOF
#!/bin/sh
if [ \$# = 2 ] && [ "\$1" = describe ] && [ "\$2" = HEAD ] && [ -n "\${GIT_DIR:-}" ] &&
    [ "\$(cd -- "\$GIT_DIR/.." 2>/dev/null && pwd -P)" = '$source_dir' ]; then
    printf '%s\n' '$WINE_DESCRIBE'
    exit 0
fi
exec '$real_git' "\$@"
EOF
    chmod 755 "$describe_bin/git"
    export PATH=$describe_bin:$PATH
fi

without=(--without-x --without-pulse --without-alsa
    --without-oss --without-sdl --without-opencl --without-cups --without-sane
    --without-gphoto --without-v4l2 --without-usb --without-netapi --without-krb5 --without-gssapi
    --without-pcap --without-coreaudio --disable-tests --without-inotify --without-dbus
    --without-capi --without-wayland --without-ffmpeg --without-gstreamer --without-fontconfig
    --without-opengl --without-udev --without-xinerama)

# PS5_WINE_ROOT for Wine's sources (loader/main.c's boot file), from
# ps5/paths.sh; rewritten only when it changes, so make rebuilds only then.
paths_header=$here/build/ps5_paths.h
write_paths_header() {
    local text="#define PS5_WINE_ROOT \"$prefix\""
    mkdir -p "$here/build"
    [[ -f $paths_header && $(<"$paths_header") == "$text" ]] || printf '%s\n' "$text" > "$paths_header"
}

configure() {
    local -a tools_env=() unix_env=()
    if [[ -n $tools_repro_flags ]]; then
        tools_env=(CFLAGS="-g -O2 $tools_repro_flags" CXXFLAGS="-g -O2 $tools_repro_flags")
    fi
    if [[ -n $repro_flags ]]; then
        unix_env=(CXXFLAGS="-g -O2 $cpu_flags $repro_flags")
    fi
    if [[ ! -f $freetype/include/freetype2/ft2build.h || ! -f $freetype/lib/libfreetype.so ]]; then
        printf 'missing PS5 FreeType: run ps5/build-freetype.sh before configuring Wine\n' >&2
        exit 1
    fi
    if [[ $(uname -s) == Darwin ]]; then
        if [[ ! -f $host_freetype/include/freetype2/ft2build.h || ! -f $host_freetype/lib/libfreetype.dylib ]]; then
            printf 'missing host FreeType: brew install freetype (or set HOST_FREETYPE)\n' >&2
            exit 1
        fi
        host_freetype_cflags="-I$host_freetype/include/freetype2"
        host_freetype_libs="-L$host_freetype/lib -lfreetype"
    else
        host_freetype_cflags=$(pkg-config --cflags freetype2)
        host_freetype_libs=$(pkg-config --libs freetype2)
    fi
    if [[ ! -f $gnutls/include/gnutls/gnutls.h || ! -f $gnutls/lib/libgnutls.so ]]; then
        printf 'missing PS5 GnuTLS: run ps5/build-gnutls.sh before configuring Wine\n' >&2
        exit 1
    fi
    write_paths_header
    if [[ ! -x $src/configure || $src/configure.ac -nt $src/configure ]]; then
        # Proton's wine tree does not commit generated files (wine/autogen.sh).
        (cd "$src" && XDG_CACHE_HOME=${TMPDIR:-/tmp}/wine-xdg ./autogen.sh)
    fi
    mkdir -p "$tools" "$cross"
    # Only native generators run here. Linux x86-64 otherwise defaults to
    # Wine's 32-bit host build and needlessly requires multilib libraries.
    local tools_arch=()
    [[ $(uname -s) != Linux ]] || tools_arch=(--enable-win64)
    env "${tools_env[@]}" \
        FREETYPE_CFLAGS="$host_freetype_cflags" \
        FREETYPE_LIBS="$host_freetype_libs" \
        python3 "$here/configure-stamp.py" --build "$tools" -- \
        "$src/configure" "${tools_arch[@]}" "${without[@]}" --without-vulkan --without-gnutls --with-freetype --enable-fonts --without-unwind
    make -C "$tools" -j"$jobs" __tooldeps__ > "$tools/make.log" 2>&1
    # __tooldeps__ also builds native sfnt2fon. With fonts enabled in the PS5
    # tree, its fonts/all target uses that tool for .fon files and installs
    # them alongside the source tree's prebuilt .ttf files.
    # wrc reads locale.nls from the tools tree's nls/, which __tooldeps__ does
    # not populate.
    mkdir -p "$tools/nls"
    ln -sf "$src/nls/locale.nls" "$tools/nls/locale.nls"
    # The PS5's libc carries the maths functions and the SDK has no libm; Wine
    # links -lm, so an empty libm.a stands in.
    mkdir -p "$here/build/ps5-lib"
    [[ -f $here/build/ps5-lib/libm.a ]] || "$sdk/prospero-ar" rc "$here/build/ps5-lib/libm.a"
    # --without-unwind: Wine's libunwind path expects GNU libunwind's
    # unw_context_t (a ucontext); the SDK's libunwind is LLVM's. ntdll still
    # calls _Unwind_Find_FDE, which LLVM's libunwind has, hence -lunwind.
    # WINE_CHECK_SONAME extracts only the basename from DT_NEEDED, discarding
    # our absolute FreeType, GnuTLS and Vulkan sonames. Override its cache
    # results so the Unix modules' dlopen use the console paths, not bare
    # names or the host build paths.
    env "${unix_env[@]}" python3 "$here/configure-stamp.py" --build "$cross" -- \
        "$src/configure" --host=x86_64-unknown-freebsd11 --with-wine-tools="$tools" \
        --enable-archs=i386,x86_64 --with-freetype --enable-fonts --with-gnutls --with-vulkan \
        CROSSCFLAGS="-g -O2 -D__WINE_PS5__ $cpu_flags $repro_flags" \
        CROSSLDFLAGS="${CROSSLDFLAGS:-} ${PS5_PE_LDFLAGS:-}" \
        CC="$sdk/prospero-clang" CXX="$sdk/prospero-clang++" AR="$sdk/prospero-ar" \
        RANLIB="$sdk/prospero-ranlib" STRIP="$sdk/prospero-strip" PKG_CONFIG=false \
        CFLAGS="-g -O2 -include $here/include/ps5_wine_compat.h -include $paths_header $cpu_flags $repro_flags $wine_ps5_extra_cflags" \
        FREETYPE_CFLAGS="-I$freetype/include/freetype2" \
        FREETYPE_LIBS="-L$freetype/lib -lfreetype" \
        ac_cv_lib_soname_freetype="$freetype_soname" \
        GNUTLS_CFLAGS="-I$gnutls/include" \
        GNUTLS_LIBS="-L$gnutls/lib -lgnutls" \
        ac_cv_lib_soname_gnutls="$gnutls_soname" \
        ac_cv_lib_soname_vulkan="$vulkan_soname" \
        UNIXLIB_SONAME_DIR="$prefix/lib/wine/x86_64-unix/" \
        LDFLAGS="-L$here/build/ps5-lib -lunwind -lps5platform" --without-unwind \
        --disable-amd_ags_x64 \
        --disable-wineopenxr \
        "${without[@]}"
    if ! grep -Fxq '#define HAVE_FT2BUILD_H 1' "$cross/include/config.h" ||
        ! grep -Fxq "#define SONAME_LIBFREETYPE \"$freetype_soname\"" "$cross/include/config.h"; then
        printf 'PS5 FreeType configuration is incomplete: check %s/configure.log\n' "$cross" >&2
        exit 1
    fi
    if ! grep -Fxq "#define SONAME_LIBGNUTLS \"$gnutls_soname\"" "$cross/include/config.h"; then
        printf 'PS5 GnuTLS configuration is incomplete: check %s/configure.log\n' "$cross" >&2
        exit 1
    fi
    if ! grep -Fxq "#define SONAME_LIBVULKAN \"$vulkan_soname\"" "$cross/include/config.h"; then
        printf 'PS5 Vulkan configuration is incomplete: check %s/configure.log\n' "$cross" >&2
        exit 1
    fi
    echo "configured: $cross"
}

# Apply ps5/wine/patches/*.patch, in name order, to the pinned checkout. A
# stamp in its Git directory names the applied series, so a rerun skips it and
# a changed series first restores the files the old one touched. The release
# Wine cache key includes the series, so a patched tree never reuses objects
# built from an unpatched one, or the reverse.
shopt -s nullglob
patches=("$here"/wine/patches/*.patch)
shopt -u nullglob
stamp=$(git -C "$src" rev-parse --path-format=absolute --git-path forever-patches)
series=$( (( ${#patches[@]} )) && cat "${patches[@]}" | git hash-object --stdin || echo none)
if [[ $(cat "$stamp" 2>/dev/null) != "$series" ]]; then
    if [[ -f $stamp ]]; then
        # Refresh first: checkout rewrites stat-dirty files too, and new mtimes
        # would make an incremental build recompile unchanged sources.
        git -C "$src" update-index -q --refresh || true
        git -C "$src" checkout -- .
    elif ! git -C "$src" diff --quiet HEAD; then
        printf '%s has local changes; ps5/wine/patches applies only to a clean pinned checkout\n' \
            "$src" >&2
        exit 1
    fi
    rm -f "$stamp"
    for patch in "${patches[@]}"; do
        printf 'applying %s\n' "${patch##*/}"
        git -C "$src" apply "$patch"
    done
    printf '%s\n' "$series" > "$stamp"
fi
[[ ${1:-} != patch ]] || exit 0

if [[ ${1:-} == configure ]]; then
    configure
    exit 0
fi
[[ -f $cross/Makefile ]] || configure
write_paths_header
targets=("$@")
(( ${#targets[@]} )) || targets=(loader/all server/all dlls/ntdll/all)
# The console's dynamic loader finds a DT_NEEDED shared object by its path,
# not by a bare soname matched against loaded objects: win32u.so, ws2_32.so
# and the other Unix modules, whose DT_NEEDED is ntdll.so, failed to load
# (ps5/probes/dl). So configure gives each module its installed path as soname
# (UNIXLIB_SONAME_DIR), as the SDK's samples/hello_so does, and modules
# linked with -lntdll record that path.
make -C "$cross" -j"$jobs" -k "${targets[@]}" > "$cross/make.log" 2>&1 && status=0 || status=$?

# Reuse one shared-object build for payload servers (broker-started loaders)
# and the title's private server thread. It contains the same server objects
# as server/wineserver, with main renamed wineserver_main; allocation/TLS
# wrappers select the title path at runtime and preserve native payload calls.
if [[ -f $cross/server/wineserver ]]; then
    objs=()
    for obj in "$cross"/server/*.o; do
        [[ $obj == */main.o || $obj == */main-so.o ]] || objs+=("$obj")
    done
    "$sdk/prospero-objcopy" --redefine-sym main=wineserver_main \
        "$cross/server/main.o" "$cross/server/main-so.o"
    "$sdk/prospero-clang" -shared -o "$cross/server/wineserver.so" -Wl,-Bsymbolic \
        -Wl,-soname,wineserver.so "${objs[@]}" "$cross/server/main-so.o" \
        -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free \
        -Wl,--wrap=pthread_key_create,--wrap=pthread_getspecific,--wrap=pthread_setspecific \
        -L"$here/build/ps5-lib" -lunwind -lps5platform >> "$cross/make.log" 2>&1 || status=$?
fi
if [[ -n ${WINE_DESCRIBE:-} && -f $version_c &&
      $(<"$version_c") != "const char wine_build[] = \"$WINE_DESCRIBE\";" ]]; then
    printf 'ntdll version string is not WINE_DESCRIBE: %s\n' "$(<"$version_c")" >&2
    status=1
fi
grep -E "error:|\*\*\*|undefined symbol" "$cross/make.log" | sort | uniq -c | sort -rn | head -40 || true
if [[ -n ${WINE_CCACHE_DIR:-} ]]; then
    "$ccache" --print-stats | awk -F'\t' '
        $1 ~ /^(direct_cache_hit|preprocessed_cache_hit)$/ { hit += $2 }
        $1 == "cache_miss" { miss = $2 }
        END { printf "WINE_CCACHE hits=%d misses=%d\n", hit, miss }'
fi
echo "make status $status, log $cross/make.log"
exit "$status"
