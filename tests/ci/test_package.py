"""Check package identity, binary architecture and archive integrity."""
import importlib.util
import json
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ringecho_package", ROOT / "scripts/ci/package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def binary(platform, arch):
    data = bytearray(128)
    if platform == "linux":
        data[:4] = b"\x7fELF"
        bits, machine = package.MACHINES[platform][arch]
        data[4:6] = bytes([bits, 1])
        struct.pack_into("<H", data, 18, machine)
    elif platform == "windows":
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 68, package.MACHINES[platform][arch])
    else:
        data[:4] = b"\xcf\xfa\xed\xfe"
        struct.pack_into("<I", data, 4, package.MACHINES[platform][arch])
    return data


class PackageTests(unittest.TestCase):
    def test_all_eight_architectures_are_identified_and_mismatches_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "program"
            for platform, architectures in package.MACHINES.items():
                for arch in architectures:
                    with self.subTest(platform=platform, arch=arch):
                        path.write_bytes(binary(platform, arch))
                        package.validate_binary(path, platform, arch)
                        for other in architectures:
                            if other != arch:
                                with self.assertRaises(ValueError):
                                    package.validate_binary(path, platform, other)

    def test_truncated_or_invalid_pe_offset_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "program.exe"
            path.write_bytes(b"MZ")
            with self.assertRaises(ValueError):
                package.validate_binary(path, "windows", "x64")
            data = binary("windows", "x64")
            struct.pack_into("<I", data, 60, 0xFFFFFFFF)
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                package.validate_binary(path, "windows", "x64")

    def test_unsafe_and_invalid_version_names_are_rejected(self):
        for value in ("../1.0.0", "1.0.0\nanything", "1.2", "1.2.70000", "1.2.3;whoami"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                package.version_string(value)
        for value in ("1.2.3", "0.0.0-dev.123.abcdef0", "1.2.3-rc.1"):
            self.assertEqual(package.version_string(value), value)

    def test_archive_contains_exact_tools_manifest_and_executable_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            build.mkdir()
            for tool in package.TOOLS:
                (build / tool).write_bytes(binary("linux", "x64"))
            (build / "stale-file").write_text("exclude this", encoding="utf-8")
            payload = root / "ringecho-1.2.3-linux-x64"
            package.prepare(build, payload, "linux", "x64", "1.2.3", "a" * 40)
            archive = package.archive(payload, root, "linux", 1234567890)
            first_hash = package.digest(archive)
            package.archive(payload, root, "linux", 1234567890)
            self.assertEqual(first_hash, package.digest(archive))
            manifest = json.loads((payload / "manifest.json").read_text(encoding="utf-8"))
            for name, digest in manifest["files"].items():
                self.assertEqual(package.digest(payload / name), digest)
            with tarfile.open(archive) as opened:
                names = opened.getnames()
                self.assertFalse(any("stale-file" in name for name in names))
                for tool in package.TOOLS:
                    member = opened.getmember(f"{payload.name}/bin/{tool}")
                    self.assertEqual(member.mode, 0o755)
                    self.assertEqual(member.uid, 0)
            package.write_checksums(root)
            sums = (root / "SHA256SUMS").read_text(encoding="utf-8")
            self.assertIn(f"{first_hash}  {archive.name}", sums)
            self.assertNotIn("SHA256SUMS", sums)

    def test_missing_tool_prevents_packaging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                package.prepare(root, root / "payload", "linux", "x64", "1.0.0", "a" * 40)


if __name__ == "__main__":
    unittest.main()
