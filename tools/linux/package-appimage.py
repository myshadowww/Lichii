#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package a recorded Linux desktop installation without modifying it."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request


LINUXDEPLOY_SHA256 = "df908b19f1ed1cdafa79508d415aa12d6c56ce4d56d4149aa8c09286718706e5"
LINUXDEPLOY_URL = (
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/"
    "linuxdeploy-x86_64.AppImage"
)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def text_section_sha256(path):
    sections = run("readelf", "--wide", "--sections", str(path))
    match = re.search(r"\[\s*\d+\]\s+\.text\s+\S+\s+\S+\s+(\S+)\s+(\S+)", sections)
    if not match:
        raise RuntimeError(f"No executable text section: {path}")
    offset, size = (int(value, 16) for value in match.groups())
    with path.open("rb") as stream:
        stream.seek(offset)
        return hashlib.sha256(stream.read(size)).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-root", type=Path, required=True,
                        help="Installation containing build-info.json and usr/bin/lichii")
    parser.add_argument("--output", type=Path, required=True, help="New output directory")
    parser.add_argument("--tools-dir", type=Path, required=True, help="linuxdeploy download cache")
    parser.add_argument("--qmake", default="qmake6")
    parser.add_argument("--source-dir", type=Path,
                        help="Matching source checkout or archive with a GIT-COMMIT file")
    args = parser.parse_args()
    if os.uname().machine != "x86_64":
        parser.error("This packager currently supports Linux x86_64 only")
    repo = Path(__file__).resolve().parents[2]
    source = args.source_dir.resolve() if args.source_dir else repo
    app = args.app_root.resolve()
    info = json.loads((app / "build-info.json").read_text())
    variant = info.get("cpu_variant", "generic")
    targets = {"generic": "x86-64", "avx512": "x86-64-v4"}
    if variant not in targets:
        parser.error("Unknown CPU variant in build-info.json")
    if info.get("compiler_target", targets[variant]) != targets[variant]:
        parser.error("CPU variant does not match the recorded compiler target")
    revision = info["head"]
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", info["date"]):
        parser.error("build-info.json must record its build date as YYYY-MM-DD")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        parser.error("build-info.json must record a full source commit")
    if (source / ".git").exists():
        git = ("git", "-c", f"safe.directory={source}", "-C", str(source))
        run(*git, "cat-file", "-e", revision + "^{commit}")
        switches = run(*git, "show", revision + ":dist/nx2_switches.txt")
    else:
        if (source / "GIT-COMMIT").read_text().strip() != revision:
            parser.error("Source archive revision does not match the installation")
        switches = (source / "dist/nx2_switches.txt").read_text()
    original = app / "usr/bin/lichii"
    if sha256(original) != info["sha256"]:
        parser.error("Installed executable does not match build-info.json")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Output directory must be empty")
    tool_dir = args.tools_dir.resolve()
    tool_dir.mkdir(parents=True, exist_ok=True)
    deploy = tool_dir / "linuxdeploy-x86_64.AppImage"
    if not deploy.exists():
        urllib.request.urlretrieve(LINUXDEPLOY_URL, deploy)
    if sha256(deploy) != LINUXDEPLOY_SHA256:
        parser.error("linuxdeploy checksum mismatch; review and update the tool pin")
    deploy.chmod(0o755)
    extracted = tool_dir / ("linuxdeploy-" + LINUXDEPLOY_SHA256[:12])
    if not (extracted / "squashfs-root/usr/bin/linuxdeploy-plugin-appimage").exists():
        extracted.mkdir(exist_ok=True)
        with (extracted / "extract.log").open("w") as log:
            subprocess.run([str(deploy), "--appimage-extract"], cwd=extracted,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    appdir = output / "AppDir"
    bindir = appdir / "usr/bin"
    bindir.mkdir(parents=True)
    for name in ("lichii", "nx2-precompile"):
        shutil.copy2(app / "usr/bin" / name, bindir / name)
    (bindir / "nx2_switches.txt").write_text(switches.rstrip() + "\n")
    # Copy only runtime files. Profiles, shaders extracted from games, downloaded
    # models, and recipes in the local installation are not distribution assets.
    plugins = Path(run(args.qmake, "-query", "QT_INSTALL_PLUGINS"))
    groups = {
        "platforms": ["libqxcb.so", "libqwayland*.so", "libqoffscreen.so", "libqminimal.so"],
        "imageformats": ["libqgif.so", "libqico.so", "libqjpeg.so", "libqsvg.so", "libqwebp.so"],
        "platforminputcontexts": ["libcomposeplatforminputcontextplugin.so", "libibusplatforminputcontextplugin.so"],
        "xcbglintegrations": ["*.so"],
        "wayland-decoration-client": ["*.so"],
        "wayland-graphics-integration-client": ["*.so"],
        "wayland-shell-integration": ["*.so"],
        "tls": ["*.so"],
    }
    for group, patterns in groups.items():
        for pattern in patterns:
            for plugin in (plugins / group).glob(pattern):
                target = appdir / "usr/plugins" / group / plugin.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(plugin, target)
    if not (appdir / "usr/plugins/platforms/libqxcb.so").exists():
        parser.error("Qt's X11 platform plugin is required")
    (bindir / "qt.conf").write_text("[Paths]\nPrefix=..\nPlugins=plugins\n")
    icon = output / "io.github.zortos293.lichii.png"
    shutil.copy2(source / "dist/lichii.png", icon)
    env = os.environ.copy()
    env.update(APPIMAGE_EXTRACT_AND_RUN="1", NO_STRIP="1", ARCH="x86_64")
    # The local installation may carry matching Qt libraries outside /usr/lib.
    qt_libs = run(args.qmake, "-query", "QT_INSTALL_LIBS")
    env["LD_LIBRARY_PATH"] = str(app / "usr/lib") + ":" + qt_libs + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    command = [str(deploy), "--appdir", str(appdir),
               "--deploy-deps-only", str(bindir),
               "--desktop-file", str(source / "dist/io.github.zortos293.lichii.desktop"),
               "--icon-file", str(icon),
               "--custom-apprun", str(repo / "tools/linux/AppRun")]
    # linuxdeploy does not recurse through the platform/plugin subdirectories.
    for plugin in sorted((appdir / "usr/plugins").rglob("*.so")):
        command += ["--deploy-deps-only", str(plugin)]
    # These redistributable runtime libraries are not present on every desktop,
    # but linuxdeploy's default exclusion list otherwise leaves them unbundled.
    dependency_paths = dict(re.findall(r"^\s*(\S+) => (/\S+)",
                                      run("ldd", str(original), env=env), re.M))
    for library in ("libOpenGL.so.0", "libOpenCL.so.1", "libjack.so.0",
                    "libharfbuzz.so.0", "libfribidi.so.0"):
        if library in dependency_paths:
            command += ["--library", dependency_paths[library]]
    # Driver libraries must come from the user's installed driver.
    for library in ("libcuda.so*", "libnvidia-*.so*", "libvulkan.so*"):
        command += ["--exclude-library", library]
    with (output / "linuxdeploy.log").open("w") as log:
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    if variant == "avx512":
        (appdir / "required-isa").write_text("x86-64-v4\n")
    licenses = appdir / "usr/share/licenses/lichii"
    licenses.mkdir(parents=True)
    shutil.copy2(source / "LICENSE.txt", licenses / "LICENSE.txt")
    shutil.copy2(source / "externals/nxformat/LICENSE", licenses / "nxformat-LICENSE")
    for name in ("LICENSE", "NOTICE", "UPSTREAM.json"):
        shutil.copy2(source / "externals/opendlss-nr" / name, licenses / ("opendlss-nr-" + name))
    qt_root = Path(run(args.qmake, "-query", "QT_INSTALL_PREFIX"))
    if (qt_root / "licenses").is_dir():
        shutil.copytree(qt_root / "licenses", licenses / "qt", dirs_exist_ok=True)
    qt_version = run(args.qmake, "-query", "QT_VERSION")
    qt_series = ".".join(qt_version.split(".")[:2])
    (licenses / "Qt-sources.txt").write_text(
        f"This AppImage includes Qt {qt_version} libraries and plugins from the Qt Project.\n"
        f"Qt source archives: https://download.qt.io/archive/qt/{qt_series}/{qt_version}/submodules/\n"
        "Component copyright and license notices are included in those source archives.\n"
        "GPL and LGPL license texts are included in the common/ directory.\n")
    # Preserve the distribution's copyright records alongside bundled libraries.
    dependencies = sorted(set(re.findall(r"^Deploying shared library (.+)$",
                                         (output / "linuxdeploy.log").read_text(), re.M)))
    package_records = {}
    if shutil.which("dpkg-query"):
        for dependency in dependencies:
            result = subprocess.run(["dpkg-query", "-S", str(Path(dependency).resolve())],
                                    text=True, capture_output=True)
            if result.returncode == 0:
                package = result.stdout.split(": ", 1)[0]
                version = run("dpkg-query", "-W", "-f=${Version}", package)
                package_records[package] = version
                copyright_file = Path("/usr/share/doc") / package.split(":")[0] / "copyright"
                if copyright_file.is_file():
                    shutil.copy2(copyright_file, licenses / (package.replace(":", "-") + "-copyright"))
    if Path("/usr/share/common-licenses").is_dir():
        shutil.copytree("/usr/share/common-licenses", licenses / "common", dirs_exist_ok=True)
    versions = set()
    isa = set()
    for path in appdir.rglob("*"):
        if path.is_file():
            with path.open("rb") as stream:
                is_elf = stream.read(4) == b"\x7fELF"
            if is_elf:
                versions.update(re.findall(r"Name: GLIBC_([0-9.]+)", run("readelf", "--version-info", str(path))))
                notes = subprocess.run(["readelf", "--notes", str(path)],
                                       text=True, capture_output=True)
                # Ubuntu 24.04's readelf returns 1 for SDL's newer FDO dlopen
                # metadata, while still decoding the GNU ISA requirement notes.
                legacy_fdo_note = (not notes.stderr and
                                   "Unknown note type: (0x407c0c0a)" in notes.stdout)
                if notes.returncode and not legacy_fdo_note:
                    raise RuntimeError(f"Cannot inspect ELF notes for {path}: {notes.stderr}")
                isa.update(re.findall(r"x86 ISA needed: ([^\n]+)", notes.stdout))
    minimum_glibc = max(versions, key=lambda v: tuple(map(int, v.split("."))))
    code_hash = text_section_sha256(original)
    if code_hash != text_section_sha256(bindir / "lichii"):
        raise RuntimeError("Packaging changed the executable's code section")
    manifest = {
        "name": "Lichii Linux desktop", "source_commit": revision,
        "source_url": "https://github.com/myshadowww/Lichii/tree/" + revision,
        "input_executable_sha256": info["sha256"],
        "packaged_executable_sha256": sha256(bindir / "lichii"),
        "minimum_glibc": minimum_glibc, "elf_isa_requirements": sorted(isa),
        "cpu_variant": variant, "compiler_target": targets[variant],
        "required_cpu_isa": targets[variant],
        "ctest": info.get("ctest", "not_recorded"),
        "qt_version": qt_version,
        "configuration": info.get("configuration", "See the source revision's build instructions"),
        "linuxdeploy_sha256": LINUXDEPLOY_SHA256,
        "packager_sha256": sha256(Path(__file__).resolve()),
        "launcher_sha256": sha256(repo / "tools/linux/AppRun"),
        "executable_text_sha256": code_hash, "distribution_packages": package_records,
        "packaging_note": "ELF library search paths changed by linuxdeploy; executable code was not rebuilt.",
    }
    suffix = "-avx512" if variant == "avx512" else ""
    metadata = output / f"build-info{suffix}.json"
    metadata.write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.copy2(metadata, appdir / "build-info.json")
    filename = f"Lichii-desktop-{info['date']}-{revision[:10]}-linux-x86_64{suffix}.AppImage"
    env.update(LDAI_VERSION=f"desktop-{info['date']}-{revision[:10]}",
               LDAI_OUTPUT=str(output / filename), OUTPUT=str(output / filename))
    with (output / "appimage.log").open("w") as log:
        subprocess.run([str(extracted / "squashfs-root/usr/bin/linuxdeploy-plugin-appimage"),
                        "--appdir", str(appdir)],
                       cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    if sha256(bindir / "lichii") != manifest["packaged_executable_sha256"]:
        raise RuntimeError("Executable changed after the package manifest was created")
    artifact = output / filename
    artifact.chmod(0o755)
    (output / "SHA256SUMS").write_text(
        f"{sha256(artifact)}  {artifact.name}\n{sha256(metadata)}  {metadata.name}\n")
    print(json.dumps({"appimage": str(artifact), "minimum_glibc": minimum_glibc,
                      "source_commit": revision, "bytes": artifact.stat().st_size}, indent=2))


if __name__ == "__main__":
    main()
