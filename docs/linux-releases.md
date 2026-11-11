# Linux AppImage releases

Download builds from [myshadowww/Lichii releases](https://github.com/myshadowww/Lichii/releases).
The `desktop` branch carries our Linux integration; dated `desktop-*` tags identify
the source used for a release. The AppImage contains the GUI, native shader
preparation helper, Qt plugins, runtime dependencies, and a `build-info.json` record.
Optional DLSS runtime and neural model files are managed through the application.
Games, keys, firmware, saves, downloaded models, and game shader recipes are excluded.

## Running and verifying a release

Download the `.AppImage`, `build-info.json`, and `SHA256SUMS` into the same directory:

```sh
sha256sum --check SHA256SUMS
chmod +x Lichii-*.AppImage
./Lichii-*.AppImage
# Systems without FUSE:
./Lichii-*.AppImage --appimage-extract-and-run
```

Check the release's glibc and hardware requirements. Ubuntu 24.04 builds target
generic x86_64 and use glibc 2.39; the packager measures the bundled ELF requirements
and records them in `build-info.json`. GPU drivers and normal system graphics/audio
libraries remain host dependencies. Qt is included. Native DLSS requires compatible
NVIDIA hardware and drivers; see [the Linux DLSS guide](user/DLSSLinux.md).

AppImages use the normal Lichii profile unless XDG directories are overridden.
They do not reproduce a developer's game-specific desktop launcher or copy its saves.

## Publishing future builds

Push a new `desktop-YYYY-MM-DD` tag, with an optional suffix for another build on the
same day, to this fork. The **Linux desktop AppImage** workflow builds and tests that
tag on Ubuntu 24.04, packages it, checks the embedded provenance and dependencies
in a clean runtime container without Qt, opens an isolated offscreen GUI, and
publishes an experimental GitHub release with
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

`tools/linux/build-appimage.sh SOURCE WORK REVISION DATE` installs Clang 23, the
pinned CMake/Qt installers and Qt 6.8.3, builds the source, runs CTest, and writes
`WORK/installed`.
Run it as root inside a disposable Ubuntu 24.04 container; it installs system
packages and must not be run directly on a personal workstation. Use `BUILD_JOBS`
to control parallelism. `SOURCE` can be a checkout or a `git archive` of `REVISION`.

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

Publish the validated `.AppImage`, `SHA256SUMS`, and `build-info.json` against the
matching source tag. Keep full build and packaging logs locally or as CI artifacts;
release assets contain no user profile or machine-specific installation manifest.
