"""Windows native backend: COFF object output and (best effort) execution.

Runs only on a Windows host. The target triple is taken from REO_NATIVE_TRIPLE
(the x86 package smoke job builds the i686 object, the x64 job the x86-64 one,
the arm64 job the arm64 one). Object emission is always checked; execution via
a C harness runs when the host architecture matches the target and a usable
toolchain is present.
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
SUPPORTED = platform.system() == "Windows"

DEFAULT_TRIPLE = ("aarch64-pc-windows-msvc" if MACHINE == "ARM64"
                  else "i686-pc-windows-msvc" if MACHINE in ("x86", "i386")
                  else "x86_64-pc-windows-msvc")
TRIPLE = os.environ.get("REO_NATIVE_TRIPLE", DEFAULT_TRIPLE)
EXPECTED_MACHINE = {"x86_64": 0x8664, "i686": 0x14C, "aarch64": 0xAA64}.get(TRIPLE.split("-")[0], 0)
HOST_NATIVE = ((TRIPLE.startswith("x86_64") and MACHINE in ("AMD64", "x86_64"))
               or (TRIPLE.startswith("aarch64") and MACHINE == "ARM64"))


def find_cc():
    for name in ("cl", "clang", "clang.exe", "gcc"):
        path = shutil.which(name)
        if path:
            return name
    return None


@unittest.skipUnless(SUPPORTED, "COFF tests require a Windows host")
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
        self.assertEqual(struct.unpack_from("<H", data, 0)[0], EXPECTED_MACHINE)
        self.assertEqual(struct.unpack_from("<H", data, 2)[0], 1)
        self.assertIn(b"answer", data)
        _, obj2 = self.build(source, name="again")
        self.assertEqual(obj.read_bytes(), obj2.read_bytes())

    def test_runtime_fault_via_interop(self):
        if self.cc is None or not HOST_NATIVE:
            self.skipTest("no matching native C toolchain")
        source = "fn dv(a: i64, b: i64) -> i64 { return a / b; }"
        result, obj = self.build(source, name="dv")
        self.assert_ok(result)
        c = self.root / "fault.c"
        c.write_text("#include <stdint.h>\nint64_t dv(int64_t,int64_t);\n"
                     "int main(void){ volatile int64_t x = dv(1,0); (void)x; return 0; }\n")
        exe = self.root / "fault.exe"
        link = self.command([self.cc, c, obj, "-o", exe])
        if link.returncode != 0:
            self.skipTest("C toolchain could not link: " + link.stdout + link.stderr)
        self.assertNotEqual(self.command([exe]).returncode, 0)

    def test_abi_interop(self):
        if self.cc is None or not HOST_NATIVE:
            self.skipTest("no matching native C toolchain")
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
