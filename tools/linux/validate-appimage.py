#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Lichii contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify release checksums, embedded provenance, dependencies, and Qt startup."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--ubuntu-baseline", action="store_true",
                        help="Require glibc <= 2.39 and ELF requirements within the declared CPU target")
    parser.add_argument("--allow-unsupported-cpu", action="store_true",
                        help="Check the package and launcher guard but skip GUI execution on an unsupported CPU")
    parser.add_argument("--report", type=Path, help="Write the validation result as JSON")
    args = parser.parse_args()
    package = args.package.resolve()
    images = list(package.glob("*.AppImage"))
    if len(images) != 1:
        parser.error("Expected exactly one AppImage")
    image = images[0]
    subprocess.run(["sha256sum", "--check", "SHA256SUMS"], cwd=package, check=True)
    metadata = list(package.glob("build-info*.json"))
    if len(metadata) != 1:
        parser.error("Expected exactly one build record")
    manifest = json.loads(metadata[0].read_text())
    variant = manifest.get("cpu_variant", "generic")
    if variant not in ("generic", "avx512"):
        raise RuntimeError("Unknown CPU variant")
    target = "x86-64-v4" if variant == "avx512" else "x86-64"
    if manifest.get("required_cpu_isa", target) != target:
        raise RuntimeError("CPU variant and instruction-set requirement disagree")
    cpu_supported = True
    if variant == "avx512":
        loader = subprocess.check_output(["/lib64/ld-linux-x86-64.so.2", "--help"],
                                         env={**os.environ, "LC_ALL": "C"}, text=True)
        cpu_supported = "x86-64-v4 (supported, searched)" in loader
        if not cpu_supported and not args.allow_unsupported_cpu:
            raise RuntimeError("AVX-512 startup validation requires an x86-64-v4 CPU")
    if args.ubuntu_baseline:
        if tuple(map(int, manifest["minimum_glibc"].split("."))) > (2, 39):
            raise RuntimeError("Package exceeds the Ubuntu 24.04 glibc baseline")
        if variant == "generic" and any("x86-64-v" in value for value in manifest["elf_isa_requirements"]):
            raise RuntimeError("Package contains libraries requiring a newer x86_64 instruction set")
    with tempfile.TemporaryDirectory(prefix="lichii-appimage-check-") as temporary:
        root = Path(temporary)
        subprocess.run([str(image), "--appimage-extract"], cwd=root,
                       stdout=subprocess.DEVNULL, check=True)
        extracted = root / "squashfs-root"
        if json.loads((extracted / "build-info.json").read_text()) != manifest:
            raise RuntimeError("Embedded build record differs from the release record")
        marker = extracted / "required-isa"
        if variant == "avx512":
            if marker.read_text().strip() != target:
                raise RuntimeError("AVX-512 launcher CPU guard is missing or incorrect")
        elif marker.exists():
            raise RuntimeError("Standard build must not require AVX-512")
        with (extracted / "usr/bin/lichii").open("rb") as binary:
            digest = hashlib.file_digest(binary, "sha256").hexdigest()
        if digest != manifest["packaged_executable_sha256"]:
            raise RuntimeError("Packaged executable checksum mismatch")
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(("EDEN_NX2_", "QT_", "DLSSNR_")) or key in (
                "LD_LIBRARY_PATH", "LD_PRELOAD", "APPDIR", "APPIMAGE", "LICHII_NGX_DIR"
            ):
                env.pop(key, None)
        env["LD_LIBRARY_PATH"] = str(extracted / "usr/lib")
        for path in (extracted / "usr").rglob("*"):
            if not path.is_file():
                continue
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
            result = subprocess.run(["ldd", str(path)], env=env, text=True, capture_output=True)
            if "not found" in result.stdout or "not found" in result.stderr:
                raise RuntimeError(f"Unresolved dependencies for {path.name}:\n{result.stdout}{result.stderr}")
            for line in result.stdout.splitlines():
                if "=> /" in line:
                    resolved = line.split("=> ", 1)[1].split(" (", 1)[0]
                    if not resolved.startswith((str(extracted) + "/", "/usr/lib/", "/usr/lib64/",
                                                "/lib/", "/lib64/", "/opt/cuda/")):
                        raise RuntimeError(f"Dependency escapes to the build environment: {resolved}")
        env.pop("LD_LIBRARY_PATH")
        for name in ("CONFIG", "DATA", "CACHE", "STATE"):
            directory = root / name.lower()
            directory.mkdir()
            env[f"XDG_{name}_HOME"] = str(directory)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        env.update(XDG_RUNTIME_DIR=str(runtime), QT_QPA_PLATFORM="offscreen",
                   QT_DEBUG_PLUGINS="1", QT_OPENGL="software", DLSSNR_DISABLE="1",
                   VK_LOADER_LAYERS_DISABLE="*")
        if variant == "avx512":
            denied_env = {**env, "GLIBC_TUNABLES": "glibc.cpu.hwcaps=-AVX512F"}
            denied = subprocess.run([str(extracted / "AppRun")], env=denied_env,
                                    text=True, capture_output=True, timeout=10)
            if denied.returncode != 78 or "requires an x86-64-v4 CPU" not in denied.stderr:
                raise RuntimeError("AVX-512 launcher did not reject an unsupported CPU")
        if not cpu_supported:
            report_result(args, manifest, variant, "skipped_unsupported_cpu")
            return
        bundled_qt_loaded = False
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen([str(image), "--appimage-extract-and-run"],
                                       cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                process.wait(timeout=10)
                raise RuntimeError(f"GUI exited during startup with status {process.returncode}")
            except subprocess.TimeoutExpired:
                # Release Qt builds may suppress plugin debug messages. Inspect
                # only this test's process group to prove the bundled UI loaded.
                for proc in Path("/proc").glob("[0-9]*"):
                    try:
                        if os.getpgid(int(proc.name)) != process.pid:
                            continue
                        executable = (proc / "exe").resolve()
                        if executable.name != "lichii":
                            continue
                        maps = (proc / "maps").read_text()
                        bundle_usr = str(executable.parent.parent) + "/"
                        qt_paths = [line.split()[-1] for line in maps.splitlines()
                                    if "libQt6" in line or "libqoffscreen.so" in line]
                        bundled_qt_loaded = (
                            any("libqoffscreen.so" in path for path in qt_paths)
                            and any("libQt6Widgets.so" in path for path in qt_paths)
                            and all(path.startswith(bundle_usr) for path in qt_paths))
                        if bundled_qt_loaded:
                            break
                    except (FileNotFoundError, ProcessLookupError, PermissionError):
                        continue
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
            log.seek(0)
            startup = log.read()
        if not bundled_qt_loaded:
            raise RuntimeError("Bundled Qt Widgets/offscreen plugin did not load:\n" + startup[-6000:])
        for failure in ("error while loading shared libraries", "symbol lookup error",
                        "Could not load the Qt platform plugin"):
            if failure in startup:
                raise RuntimeError("GUI startup failed:\n" + startup[-6000:])
    report_result(args, manifest, variant, "passed")


def report_result(args, manifest, variant, startup):
    report = {"source_commit": manifest["source_commit"], "cpu_variant": variant,
              "checksums": "passed", "embedded_manifest": "passed",
              "dependencies": "passed", "offscreen_startup": startup,
              "unsupported_cpu_guard": "passed" if variant == "avx512" else "not_required",
              "gameplay_tested": False}
    rendered = json.dumps(report, indent=2) + "\n"
    if args.report:
        args.report.write_text(rendered)
    print(rendered, end="")


if __name__ == "__main__":
    main()
