#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Run as root in a fresh Ubuntu 24.04 container; outputs stay in the work directory.
set -euo pipefail
source_dir=$(realpath "${1:?source directory}")
work_dir=$(realpath -m "${2:?work directory}")
revision=${3:?full source commit}
build_date=${4:?release date YYYY-MM-DD}
variant=${5:-generic}
jobs=${BUILD_JOBS:-8}
[[ $revision =~ ^[0-9a-f]{40}$ && $build_date =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]
case "$variant" in
    generic) cpu_target=x86-64 ;;
    avx512) cpu_target=x86-64-v4 ;;
    *) echo "Unknown CPU variant: $variant (expected generic or avx512)" >&2; exit 1 ;;
esac
mkdir -p "$work_dir"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    autoconf automake ca-certificates curl g++-13 gcc-13 git patch make \
    glslang-tools libglu1-mesa-dev libpulse-dev libtool libudev-dev \
    libxkbcommon-x11-0 mesa-common-dev nasm ninja-build pkg-config \
    libavfilter-dev libavcodec-dev libavdevice-dev libavformat-dev libswscale-dev \
    zlib1g-dev libva-dev libvdpau-dev spirv-tools spirv-headers \
    libusb-1.0-0-dev libboost-dev libboost-fiber-dev libboost-context-dev \
    libopus-dev libasound2-dev libssl-dev python3-venv \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
    libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 \
    libegl1 libopengl0 libfontconfig1 libdbus-1-3 libx11-xcb1 libxrender1 \
    libxcb-xfixes0 libxkbcommon0 libxkbcommon-dev libwayland-client0 \
    libwayland-cursor0 libwayland-egl1 desktop-file-utils file binutils
# Match the validated desktop compiler and support the source's GNU2 TLS dialect.
if ! command -v clang++-23 > /dev/null; then
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc
    printf '%s\n' 'deb https://apt.llvm.org/noble/ llvm-toolchain-noble-23 main' \
        > /etc/apt/sources.list.d/llvm.list
    apt-get update
    apt-get install -y --no-install-recommends clang-23 lld-23
fi
if [[ ! -x $work_dir/venv/bin/python ]]; then
    python3 -m venv "$work_dir/venv"
fi
"$work_dir/venv/bin/pip" install cmake==4.4.4 aqtinstall==3.3.0
qt_root="$work_dir/qt/6.8.3/gcc_64"
if [[ ! -x $qt_root/bin/qmake ]]; then
    "$work_dir/venv/bin/aqt" install-qt linux desktop 6.8.3 linux_gcc_64 \
        -m qtcharts qtwaylandcompositor -O "$work_dir/qt"
fi
printf '%s\n' "$revision" > "$source_dir/GIT-COMMIT"
printf '%s\n' desktop > "$source_dir/GIT-REFSPEC"
printf '%s\n' "desktop-$build_date" > "$source_dir/GIT-TAG"
cmake="$work_dir/venv/bin/cmake"
"$cmake" --fresh -S "$source_dir" -B "$work_dir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23 \
    '-DCMAKE_EXE_LINKER_FLAGS=-B/usr/lib/llvm-23/bin -fuse-ld=lld' -DYUZU_BUILD_PRESET=custom \
    "-DCMAKE_C_FLAGS=-march=$cpu_target -mtune=generic" \
    "-DCMAKE_CXX_FLAGS=-march=$cpu_target -mtune=generic" \
    -DBUILD_TESTING=ON -DYUZU_TESTS=ON -DYUZU_CMD=OFF -DYUZU_ROOM=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF -DCMAKE_PREFIX_PATH="$qt_root" \
    -DYUZU_USE_BUNDLED_SDL3=ON -DVulkanHeaders_FORCE_BUNDLED=ON \
    -DVulkanUtilityLibraries_FORCE_BUNDLED=ON -DENABLE_QT_TRANSLATION=ON \
    -DENABLE_DEBUG_TOOLS=ON \
    -DENABLE_NVIDIA_DLSS=ON -DENABLE_UPDATE_CHECKER=OFF -DENABLE_WERROR=OFF \
    -DUSE_DISCORD_PRESENCE=OFF -DUSE_CCACHE=OFF \
    -DLICHII_BUILD_TAG="desktop-$build_date" -DLICHII_BUILD_PLATFORM="linux-x86_64-$variant"
"$cmake" --build "$work_dir/build" --parallel "$jobs"
ctest_status=passed
if [[ $variant == avx512 ]] && ! LC_ALL=C /lib64/ld-linux-x86-64.so.2 --help | grep -Fq 'x86-64-v4 (supported, searched)'; then
    if [[ ${ALLOW_UNSUPPORTED_CPU:-0} != 1 ]]; then
        echo 'AVX-512 tests require an x86-64-v4 CPU; refusing to skip them.' >&2
        exit 1
    fi
    ctest_status=skipped_unsupported_cpu
    echo 'AVX-512 test binaries built; execution skipped because this runner lacks x86-64-v4.'
else
    "$work_dir/venv/bin/ctest" --test-dir "$work_dir/build" --output-on-failure --no-tests=error --parallel "$jobs"
fi
mkdir -p "$work_dir/installed/usr/bin" "$work_dir/installed/usr/lib"
cp "$work_dir/build/bin/lichii" \
    "$work_dir/build/bin/nx2-precompile" "$work_dir/installed/usr/bin/"
python3 - "$work_dir/installed" "$revision" "$build_date" "$variant" "$cpu_target" "$ctest_status" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
with (root / 'usr/bin/lichii').open('rb') as binary:
    digest = hashlib.file_digest(binary, 'sha256').hexdigest()
(root / 'build-info.json').write_text(json.dumps({
    'head': sys.argv[2], 'date': sys.argv[3], 'sha256': digest,
    'cpu_variant': sys.argv[4], 'compiler_target': sys.argv[5], 'ctest': sys.argv[6],
    'configuration': f'Ubuntu 24.04, Clang 23, Qt 6.8.3, {sys.argv[5]}, native Linux DLSS',
}, indent=2) + '\n')
PY
printf '\nBuild complete. CTest: %s\nInstallation: %s/installed\nQt: %s\n' "$ctest_status" "$work_dir" "$qt_root"
