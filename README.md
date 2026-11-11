<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii" width="600">
</p>

# Lichii — myshadowww's Linux fork

This is [myshadowww's independently maintained fork of Lichii](https://github.com/myshadowww/Lichii), focused on **Linux desktop support, native Linux DLSS, neural rendering, and game compatibility fixes**. Lichii is an experimental, open-source Nintendo Switch and Switch 2 emulator written in C++.

**[Download our releases](https://github.com/myshadowww/Lichii/releases)** · **[Report an issue here](https://github.com/myshadowww/Lichii/issues)** · **[Linux build and packaging guide](docs/linux-releases.md)**

The `desktop` branch contains this fork's Linux integration. Dated `desktop-*` tags identify the source used for our AppImage releases. Please bring problems with these builds to this repository; upstream maintainers are not responsible for supporting this fork. Upstream projects and contributors are credited below.

Lichii is not affiliated with Nintendo.

## Downloads and setup

Our downloads are published at **[myshadowww/Lichii releases](https://github.com/myshadowww/Lichii/releases)**. Choose one Linux x86_64 AppImage:

| Build | Filename suffix | CPU requirement |
| --- | --- | --- |
| Standard / portable | `linux-x86_64.AppImage` | No AVX-512 requirement; choose this if unsure |
| AVX-512 | `linux-x86_64-avx512.AppImage` | CPU and OS support for x86-64-v4 |

Each release records its source commit, minimum glibc version and validation results. Qt is bundled. AVX-512 may improve some workloads; a game-performance gain is not guaranteed.

Download one AppImage, its build record (`build-info.json` or `build-info-avx512.json`), and `SHA256SUMS` into the same directory:

```sh
sha256sum --check --ignore-missing SHA256SUMS
chmod +x Lichii-*.AppImage
./Lichii-*.AppImage
```

Download one variant, or use its full filename when launching if you have both.
If FUSE is unavailable, add `--appimage-extract-and-run`. See
[Linux releases](docs/linux-releases.md) for verification and build instructions,
and [native Linux DLSS](docs/user/DLSSLinux.md) for optional runtime/model setup.
Games, keys, firmware, saves, and game shader recipes are not included.

### Existing saves and settings

Lichii uses its own profile: `%APPDATA%\lichii` on Windows, and the `lichii` directories under `$XDG_DATA_HOME`, `$XDG_CONFIG_HOME` and `$XDG_CACHE_HOME` on Linux and macOS. Without XDG overrides, these are `~/.local/share/lichii`, `~/.config/lichii` and `~/.cache/lichii`. An existing portable `user/` directory still takes priority and is not imported or changed by the wizard.

On the first non-portable GUI launch, Lichii offers to import Eden and other detected emulator profiles. Back up `%APPDATA%\eden` first on Windows, or the old data, config and cache directories on Unix, and close the source emulator. Choose **Keep Old Data** to copy without changing the source, then check your saves and relaunch. Existing destination data is retained in backup directories rather than merged. Move also retains a source backup until you remove it yourself; Link deliberately shares settings and saves. Paths outside the imported data root remain shared and unchanged. An interrupted import offers recovery on the next launch. `eden-cli`, including `--help` and `--version`, refuses to initialize configuration until you import or explicitly choose **Start fresh** in the GUI.

## Compatibility and issue reports

Compatibility and performance vary by game, hardware and driver. Rendering errors, stutters and crashes remain possible. Passing automated tests does not establish that a game renders correctly, and this project makes no universal frame-rate guarantee.

Search [this fork's issues](https://github.com/myshadowww/Lichii/issues) before opening a [bug report or support question](https://github.com/myshadowww/Lichii/issues/new/choose). Include:

- The release tag or source commit and whether you used the standard or AVX-512 AppImage.
- Your Linux distribution, game version, CPU, GPU and driver version.
- Relevant settings and steps to reproduce the problem.
- A screenshot and the log from the affected session, after checking it for personal information.

Do not upload game files, keys, firmware or private account information.

The inherited **Submit compatibility report** option in the application connects to an upstream service. Use this fork's GitHub issue tracker for reports about our builds.

## Building and contributing

Start with the [Linux AppImage build guide](docs/linux-releases.md) for our release configuration. See also the [general build guide](docs/Build.md), [user documentation](docs/user/README.md), [contribution guide](CONTRIBUTING.md) and [coding guidelines](docs/Coding.md). Some inherited documentation and internal build target names still use upstream names. The GUI build target remains `yuzu`; its output is `lichii.exe` on Windows and `lichii` on other desktop platforms.

Open pull requests against this repository's `desktop` branch. Bug reports with reproduction steps, Linux testing, documentation improvements and code contributions all help. Keep changes focused and distinguish code-level checks from actual gameplay testing.

## Artwork
Art created by YOB (yobslab)
The supplied Lichii logo, icon and library artwork are in [dist/branding](dist/branding). Windows, Linux and macOS icon resources are generated from that artwork by [Generate-LichiiIcons.ps1](tools/Generate-LichiiIcons.ps1).

## Credits and license

This fork builds on [zortos293/Lichii](https://github.com/zortos293/Lichii) and the work of the Eglantine, Eden, yuzu and Sudachi projects and their contributors, including Eglantine's original developer Camille LaVey. Original copyright notices and third-party licenses are retained.

See [LICENSE.txt](LICENSE.txt) and the source files for applicable license terms.
