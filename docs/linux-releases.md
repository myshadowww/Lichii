# Linux AppImage releases

Download builds from [myshadowww/Lichii releases](https://github.com/myshadowww/Lichii/releases).
The `desktop` branch carries our Linux integration; dated `desktop-*` tags identify
the source used for a release. The AppImage contains the GUI, native shader
preparation helper, Qt plugins, runtime dependencies, and a `build-info.json` record.
Optional DLSS runtime and neural model files are managed through the application.
Games, keys, firmware, saves, downloaded models, and game shader recipes are excluded.

## Running and verifying a release

Choose one build:

| Download suffix | CPU requirement | When to choose it |
| --- | --- | --- |
| `linux-x86_64.AppImage` | Generic x86_64; no AVX-512 requirement | Default, including machines without AVX-512 |
| `linux-x86_64-avx512.AppImage` | x86-64-v4, including AVX-512 F/BW/CD/DQ/VL and all v2/v3 features | Optional optimized build for compatible CPUs |

The AVX-512 launcher checks CPU and OS support before loading the application and
directs unsupported machines to the standard build. On a glibc system, check with
`/lib64/ld-linux-x86-64.so.2 --help`: `x86-64-v4 (supported, searched)` indicates support.

AVX-512 is not a guaranteed FPS improvement. Both builds retain the emulator's
runtime CPU feature detection and JIT optimizations. The optimized build also lets
the compiler use x86-64-v4 instructions in application code; gains depend on the
CPU and workload and need a matched gameplay benchmark. Some processors reduce
frequency under AVX-512 workloads; see [Intel's guidance](https://www.intel.com/content/www/us/en/support/articles/000090746/processors.html).

Download one `.AppImage`, its `build-info.json` or `build-info-avx512.json`, and
`SHA256SUMS` into the same directory. Releases built by the workflow also include
`validation-generic.json` and `validation-avx512.json` reports.

```sh
sha256sum --check --ignore-missing SHA256SUMS
chmod +x Lichii-*.AppImage
./Lichii-*.AppImage
# Systems without FUSE:
./Lichii-*.AppImage --appimage-extract-and-run
```

The commands above assume only one AppImage is in the directory. If you download
both, launch the chosen filename explicitly.

Check the release's glibc and hardware requirements. Both variants use Ubuntu 24.04
and glibc 2.39; the packager measures the bundled ELF requirements and records them
alongside the explicit compiler target in the build record. ELF notes alone do not
reliably describe instructions emitted by the compiler. GPU drivers and normal system graphics/audio
libraries remain host dependencies. Qt is included. Native DLSS requires compatible
NVIDIA hardware and drivers; see [the Linux DLSS guide](user/DLSSLinux.md).

AppImages use the normal Lichii profile unless XDG directories are overridden.
They do not reproduce a developer's game-specific desktop launcher or copy its saves.

On Linux, the GUI and command-line frontend give NVIDIA's shader disk cache a
10 GiB limit and a separate `lichii/nvidia` directory under `XDG_CACHE_HOME`
(or `~/.cache` when that variable is missing or relative). The first run with this
directory needs to warm the driver cache again. Existing
`__GL_SHADER_DISK_CACHE_PATH` and `__GL_SHADER_DISK_CACHE_SIZE` settings take
precedence, including explicitly empty values. The directory is used only when it
can be created; no existing cache files are moved or deleted. The Vulkan log
records these settings when using NVIDIA's proprietary driver.

## Publishing future builds

Push a new `desktop-YYYY-MM-DD` tag, with an optional suffix for another build on the
same day, to this fork. The **Linux desktop AppImage** workflow builds both variants
from that tag on Ubuntu 24.04. Each package gets checksum, provenance and dependency
checks in a clean runtime container without Qt. CTest and isolated offscreen GUI
startup run when the runner supports the build's CPU target. GitHub-hosted runners
may lack AVX-512: those execution checks are explicitly recorded as
`skipped_unsupported_cpu`, and the unsupported-CPU launcher check still runs.
Compilation, packaging and other validation failures always fail the workflow.
The release includes both variants, separate build/validation records and combined
checksums. The workflow can also be dispatched manually for an existing tag.
It fails rather than replacing an existing release's assets. GPU/gameplay validation
is separate from these automated checks.

```sh
git tag -a desktop-YYYY-MM-DD -m 'Tested Linux desktop source' <tested-commit>
git push fork refs/tags/desktop-YYYY-MM-DD
```

The tag must contain the workflow for automatic tag builds. For an older tag,
dispatch the workflow from `desktop` and specify the existing tag as its input.

## Local build and packaging

`tools/linux/build-appimage.sh SOURCE WORK REVISION DATE [generic|avx512]` installs Clang 23, the
pinned CMake/Qt installers and Qt 6.8.3, builds the source, runs CTest, and writes
`WORK/installed`. The default is `generic`. Use a separate `WORK` directory per
variant. Both use `-mtune=generic`; their `-march` targets are `x86-64` and
`x86-64-v4`, respectively. Neither uses `-march=native` or additional fast-math flags.
Run it as root inside a disposable Ubuntu 24.04 container; it installs system
packages and must not be run directly on a personal workstation. Use `BUILD_JOBS`
to control parallelism. `SOURCE` can be a checkout or a `git archive` of `REVISION`.
Local AVX-512 builds require a compatible CPU to run tests. The workflow explicitly
sets `ALLOW_UNSUPPORTED_CPU=1` to allow a recorded CTest skip on other runners;
the package validator has a corresponding `--allow-unsupported-cpu` flag.

In that same Ubuntu environment, package the installation with:

```sh
python3 tools/linux/package-appimage.py \
  --app-root /work/installed --source-dir /source \
  --output /work/package --tools-dir /work/tools \
  --qmake /work/qt/6.8.3/gcc_64/bin/qmake
python3 tools/linux/validate-appimage.py --ubuntu-baseline /work/package
```

Packaging starts from the installation's recorded executable checksum. Linuxdeploy
is checksum-pinned; it bundles dependencies and adjusts ELF library search paths.
The packager checks that executable code is unchanged and records both executable
hashes. Packaging a rolling-distribution installation can raise the glibc and CPU
requirements, so public builds should use the Ubuntu environment.

Publish both validated `.AppImage` files, their build records and validation reports
against the matching source tag. Combine the per-variant `SHA256SUMS` files; metadata
filenames differ so they do not overwrite each other. Keep full build and packaging logs locally or as CI artifacts;
release assets contain no user profile or machine-specific installation manifest.
