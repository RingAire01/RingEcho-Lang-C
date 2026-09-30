"""Mach-O native backend on macOS: object format, execution and ABI.

Runs only on a macOS host, where the generated Mach-O executable runs directly
and the system `ld` links the relocatable objects.
"""
import os
from pathlib import Path
import platform
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()
MACHINE = platform.machine()
SUPPORTED = platform.system() == "Darwin" and MACHINE in ("x86_64", "arm64")
TRIPLE = "x86_64-apple-darwin" if MACHINE == "x86_64" else "aarch64-apple-darwin"
CPUTYPE = 0x01000007 if MACHINE == "x86_64" else 0x0100000C


@unittest.skipUnless(SUPPORTED, "Mach-O execution requires a macOS host")
class NativeMacOSTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reo native macho ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, args, **kwargs):
        return subprocess.run([str(a) for a in args], cwd=self.root,
                              text=True, capture_output=True, timeout=60, **kwargs)

    def build(self, source, name="program", obj=False):
        path = self.root / (name + ".reo")
        path.write_text(source, encoding="utf-8")
        output = self.root / (name + (".o" if obj else ".out"))
        args = [COMPILER, "build", path, "--backend", "native",
                "--target", TRIPLE, "-o", output]
        if obj:
            args += ["--emit", "obj"]
        return self.command(args), output

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_macho_header_and_execution(self):
        result, output = self.build("fn main() {}", name="inspect")
        self.assert_ok(result)
        self.assert_ok(self.command([output]))
        data = output.read_bytes()
        self.assertEqual(struct.unpack_from("<I", data, 0)[0], 0xFEEDFACF)
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], CPUTYPE)

    def test_object_is_deterministic(self):
        source = "fn answer() -> i64 { return 42; }"
        a, first = self.build(source, name="a", obj=True)
        b, second = self.build(source, name="b", obj=True)
        self.assert_ok(a); self.assert_ok(b)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        symbols = self.command(["nm", first])
        self.assert_ok(symbols)
        self.assertIn("_answer", symbols.stdout)

    def test_control_flow_and_integer_math(self):
        source = '''
fn factorial(n: i64) -> i64 { if n <= 1 { return 1; } return n * factorial(n - 1); }
fn sum6(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64) -> i64 { return a + b + c + d + e + f; }
fn main() {
    assert(factorial(10) == 3628800);
    assert(sum6(1,2,3,4,5,6) == 21);
    let x: i64 = 100; assert(x / 7 == 14); assert(x % 7 == 2);
    let y: i64 = -7; assert(y / 2 == -3); assert(y % 2 == -1);
    let max: u64 = 18446744073709551615u64; assert(max + 1u64 == 0u64);
}
'''
        result, exe = self.build(source)
        self.assert_ok(result)
        self.assert_ok(self.command([exe]))

    def test_runtime_failures_are_nonzero(self):
        for expression in ("assert(false)", "1 / 0", "1 % 0",
                           "(9223372036854775808u64 as i64) / -1"):
            with self.subTest(expression=expression):
                result, exe = self.build("fn main() { " + expression + "; }")
                self.assert_ok(result)
                self.assertNotEqual(self.command([exe]).returncode, 0)

    @unittest.skipIf(MACHINE == "x86_64", "x86-64 supports floating point")
    def test_floating_point_is_rejected_explicitly(self):
        result, output = self.build("fn main() { let x: f64 = 1.5; }")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("floating point", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
