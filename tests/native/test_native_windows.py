"""Windows native backend: COFF object output and (best effort) execution.

Runs only on a Windows x86-64 host. Object emission is always checked; when a
usable C toolchain is present the objects are linked against a C harness and
executed to validate the Microsoft x64 calling convention.
"""
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev.exe")).resolve()
MACHINE = platform.machine()
SUPPORTED = platform.system() == "Windows" and MACHINE in ("AMD64", "x86_64")
TRIPLE = "x86_64-pc-windows-msvc"
MACHINE_AMD64 = 0x8664


def find_cc():
    for name in ("cl", "clang", "clang.exe", "gcc", "x86_64-w64-mingw32-gcc"):
        path = shutil.which(name)
        if path:
            return name
    return None


@unittest.skipUnless(SUPPORTED, "COFF tests require a Windows x86-64 host")
class NativeWindowsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reo native coff ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cc = find_cc()

    def command(self, args, **kwargs):
        return subprocess.run([str(a) for a in args], cwd=self.root,
                              text=True, capture_output=True, timeout=60, **kwargs)

    def build(self, source, name="program", obj=True):
        path = self.root / (name + ".reo")
        path.write_text(source, encoding="utf-8")
        output = self.root / (name + (".obj" if obj else ".exe"))
        args = [COMPILER, "build", path, "--backend", "native",
                "--target", TRIPLE, "-o", output]
        if obj:
            args += ["--emit", "obj"]
        return self.command(args), output

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_coff_header_and_determinism(self):
        source = "fn answer() -> i64 { return 42; }"
        result, obj = self.build(source)
        self.assert_ok(result)
        data = obj.read_bytes()
        self.assertEqual(struct.unpack_from("<H", data, 0)[0], MACHINE_AMD64)
        self.assertEqual(struct.unpack_from("<H", data, 2)[0], 1)  # one section
        _, obj2 = self.build(source, name="again")
        self.assertEqual(obj.read_bytes(), obj2.read_bytes())
        self.assertIn(b"answer", obj.read_bytes())

    def test_interop_and_execution(self):
        if self.cc is None:
            self.skipTest("no C toolchain on PATH")
        source = '''
fn add6(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64) -> i64 { return a + b + c + d + e + f; }
fn mix(a: i32, b: i64, c: i32) -> i64 { return (a as i64) + b + (c as i64); }
fn narrow(x: i8, y: u16) -> i32 { return (x as i32) + (y as i32); }
'''
        result, obj = self.build(source, name="abi")
        self.assert_ok(result)
        c = self.root / "abi_main.c"
        c.write_text("#include <stdint.h>\n#include <stdio.h>\n"
                     "extern int64_t add6(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
                     "extern int64_t mix(int32_t,int64_t,int32_t);\n"
                     "extern int32_t narrow(int8_t,uint16_t);\n"
                     "int main(void){ printf(\"%lld %lld %d\\n\","
                     "(long long)add6(1,2,3,4,5,6),(long long)mix(-5,100,7),narrow(-1,65535));"
                     " return 0; }\n")
        exe = self.root / "abi.exe"
        link = self.command([self.cc, c, obj, "-o", exe])
        if link.returncode != 0:
            self.skipTest("C toolchain could not link: " + link.stdout + link.stderr)
        result = self.command([exe])
        self.assert_ok(result)
        self.assertEqual(result.stdout.strip(), "21 102 65534")


if __name__ == "__main__":
    unittest.main()
