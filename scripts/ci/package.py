"""Build architecture-checked portable archives and native Unix packages."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ("rev", "rem", "rvm")
MACHINES = {
    "linux": {"x86": (1, 3), "x64": (2, 62), "arm64": (2, 183)},
    "windows": {"x86": 0x14C, "x64": 0x8664, "arm64": 0xAA64},
    "macos": {"x64": 0x01000007, "arm64": 0x0100000C},
}


def validate_binary(path, platform, arch):
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) < 64:
            raise ValueError(f"二进制文件不完整：{path}")
        if platform == "linux":
            actual = (header[4], struct.unpack_from("<H", header, 18)[0])
            valid = header[:4] == b"\x7fELF" and header[5] == 1 and actual == MACHINES[platform][arch]
        elif platform == "windows":
            offset = struct.unpack_from("<I", header, 60)[0]
            if offset > path.stat().st_size - 6:
                raise ValueError(f"PE 头偏移越界：{path}")
            stream.seek(offset)
            signature = stream.read(6)
            valid = (header[:2] == b"MZ" and signature[:4] == b"PE\0\0" and
                     struct.unpack_from("<H", signature, 4)[0] == MACHINES[platform][arch])
        else:
            valid = (header[:4] == b"\xcf\xfa\xed\xfe" and
                     struct.unpack_from("<I", header, 4)[0] == MACHINES[platform][arch])
        if not valid:
            raise ValueError(f"二进制架构与 {platform}/{arch} 不一致：{path}")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_checksums(directory):
    paths = sorted(p for p in directory.iterdir() if p.is_file() and p.name != "SHA256SUMS")
    (directory / "SHA256SUMS").write_text(
        "".join(f"{digest(p)}  {p.name}\n" for p in paths), encoding="utf-8")


def version_string(value):
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?", value):
        raise ValueError("版本必须为 major.minor.patch 或带预发布后缀的版本号")
    if any(int(part) > 65535 for part in value.split("-")[0].split(".")):
        raise ValueError("版本分量不能超过 65535")
    return value


def prepare(build_dir, destination, platform, arch, version, revision):
    suffix = ".exe" if platform == "windows" else ""
    (destination / "bin").mkdir(parents=True)
    for tool in TOOLS:
        source = build_dir / (tool + suffix)
        validate_binary(source, platform, arch)
        target = destination / "bin" / source.name
        shutil.copyfile(source, target)
        target.chmod(0o755)
    for name in ("LICENSE", "README.zh.md", "BUILD.zh.md"):
        shutil.copyfile(ROOT / name, destination / name)
    shutil.copyfile(ROOT / "packaging/INSTALL.zh.md", destination / "INSTALL.zh.md")
    manifest = {"version": version, "revision": revision, "platform": platform, "arch": arch,
                "runtime_tests": "frontend-only" if arch == "x86" else "see-ci-results",
                "files": {p.relative_to(destination).as_posix(): digest(p)
                          for p in sorted(destination.rglob("*")) if p.is_file()}}
    (destination / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def archive(payload, output, platform, epoch):
    if platform == "windows":
        destination = output / f"{payload.name}.zip"
        with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as archive_file:
            for path in sorted(payload.rglob("*")):
                if path.is_file():
                    archive_file.write(path, path.relative_to(payload.parent).as_posix())
    else:
        destination = output / f"{payload.name}.tar.gz"
        with destination.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as gz:
            with tarfile.open(fileobj=gz, mode="w") as archive_file:
                for path in [payload, *sorted(payload.rglob("*"))]:
                    info = archive_file.gettarinfo(str(path), path.relative_to(payload.parent).as_posix())
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    info.mtime = epoch
                    if path.is_file():
                        with path.open("rb") as stream:
                            archive_file.addfile(info, stream)
                    else:
                        archive_file.addfile(info)
    return destination


def install_tree(payload, root, prefix):
    binary_dir = root / prefix / "bin"
    documentation = root / prefix / "share/doc/ringecho"
    shutil.copytree(payload / "bin", binary_dir)
    documentation.mkdir(parents=True)
    for path in payload.iterdir():
        if path.is_file():
            shutil.copyfile(path, documentation / path.name)


def deb_package(payload, output, arch, version, scratch):
    root = scratch / "deb-root"
    install_tree(payload, root, "usr")
    control = root / "DEBIAN"
    control.mkdir()
    # Let the platform tooling determine the actual ABI dependencies.
    (scratch / "debian").mkdir()
    (scratch / "debian/control").write_text(
        "Source: ringecho\nMaintainer: RingAire <noreply@github.com>\n\n"
        "Package: ringecho\nArchitecture: any\nDescription: RingEcho compiler tools\n", encoding="utf-8")
    result = subprocess.run(["dpkg-shlibdeps", "-O", *[f"-e{p}" for p in sorted((root / "usr/bin").iterdir())]],
                            cwd=scratch, check=True, capture_output=True, text=True)
    dependencies = next(line.split("=", 1)[1] for line in result.stdout.splitlines()
                        if line.startswith("shlibs:Depends="))
    deb_arch = {"x64": "amd64", "x86": "i386", "arm64": "arm64"}[arch]
    installed_size = (sum(p.stat().st_size for p in root.rglob("*") if p.is_file()) + 1023) // 1024
    (control / "control").write_text(
        f"Package: ringecho\nVersion: {version.replace('-', '~', 1)}\nArchitecture: {deb_arch}\n"
        f"Maintainer: RingAire <noreply@github.com>\nDepends: {dependencies}\nRecommends: gcc\n"
        f"Section: devel\nPriority: optional\nInstalled-Size: {installed_size}\n"
        "Description: RingEcho compiler, package manager and version manager\n"
        " Includes rev, rem and rvm. A system C compiler is required for native code generation.\n",
        encoding="utf-8")
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(root),
                    str(output / f"{payload.name}.deb")], check=True)


def mac_package(payload, output, arch, version, scratch):
    root = scratch / "pkg-root"
    install_tree(payload, root, "usr/local")
    identifier = "org.ringaire.ringecho"
    component = scratch / "component.pkg"
    numeric_version = version.split("-")[0]
    subprocess.run(["pkgbuild", "--root", str(root), "--identifier", identifier,
                    "--version", numeric_version, "--install-location", "/", str(component)], check=True)
    distribution = ET.Element("installer-gui-script", {"minSpecVersion": "2"})
    ET.SubElement(distribution, "title").text = "RingEcho"
    ET.SubElement(distribution, "options", {"customize": "never", "require-scripts": "false",
                                           "hostArchitectures": "x86_64" if arch == "x64" else "arm64"})
    choices = ET.SubElement(distribution, "choices-outline")
    ET.SubElement(choices, "line", {"choice": "ringecho"})
    choice = ET.SubElement(distribution, "choice", {"id": "ringecho", "visible": "false"})
    ET.SubElement(choice, "pkg-ref", {"id": identifier})
    ET.SubElement(distribution, "pkg-ref", {"id": identifier, "version": numeric_version}).text = "component.pkg"
    xml = scratch / "distribution.xml"
    ET.ElementTree(distribution).write(xml, encoding="utf-8", xml_declaration=True)
    subprocess.run(["productbuild", "--distribution", str(xml), "--package-path", str(scratch),
                    str(output / f"{payload.name}.pkg")], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=MACHINES, required=True)
    parser.add_argument("--arch", choices=("x64", "x86", "arm64"), required=True)
    parser.add_argument("--build-dir", type=Path, default=Path("target/Release"))
    parser.add_argument("--output", type=Path, default=Path("target/packages"))
    parser.add_argument("--version", type=version_string, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--epoch", type=int, default=0)
    args = parser.parse_args()
    if args.arch not in MACHINES[args.platform]:
        parser.error("不支持的平台架构组合")
    if not re.fullmatch(r"[0-9a-f]{40,64}", args.revision) or args.epoch < 0:
        parser.error("提交标识或构建时间无效")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    # A private staging directory prevents stale files from entering packages.
    with tempfile.TemporaryDirectory(prefix="ringecho-package-") as directory:
        scratch = Path(directory)
        payload = scratch / f"ringecho-{args.version}-{args.platform}-{args.arch}"
        prepare(args.build_dir.resolve(), payload, args.platform, args.arch, args.version, args.revision)
        archive(payload, args.output, args.platform, args.epoch)
        if args.platform == "linux":
            deb_package(payload, args.output, args.arch, args.version, scratch)
        elif args.platform == "macos":
            mac_package(payload, args.output, args.arch, args.version, scratch)
    write_checksums(args.output)


if __name__ == "__main__":
    main()
