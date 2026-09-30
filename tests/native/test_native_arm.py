"""ARMv7 native backend: ELF32 output, integer arithmetic, AAPCS ABI.

There is no native armv7 CI runner, so these tests execute the generated
binaries through qemu-arm and link relocatable objects with the
arm-linux-gnueabihf cross compiler when they are not running on real ARM
hardware. They are skipped when the required tools are unavailable.
"""
import os
from pathlib import Path
import platform
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()
TRIPLE = "armv7-unknown-linux-gnueabihf"
NATIVE = platform.machine() in ("armv7l", "armv6l", "arm")
QEMU = shutil.which("qemu-arm")
LD = shutil.which("arm-linux-gnueabihf-ld")
CC = shutil.which("arm-linux-gnueabihf-gcc")
SUPPORTED = (NATIVE or QEMU is not None) and LD is not None


@unittest.skipUnless(SUPPORTED, "armv7 tests require qemu-arm and arm cross binutils")
class NativeArmTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reo native arm ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ)
        if not NATIVE:
            self.env["REO_LD"] = LD

    def command(self, args, **kwargs):
        return subprocess.run([str(a) for a in args], cwd=self.root,
                              text=True, capture_output=True, timeout=60, **kwargs)

    def run_exe(self, exe):
        return self.command(([] if NATIVE else [QEMU]) + [exe])

    def build(self, source, name="program", obj=False):
        path = self.root / (name + ".reo")
        path.write_text(source, encoding="utf-8")
        output = self.root / (name + (".o" if obj else ".elf"))
        args = [COMPILER, "build", path, "--backend", "native",
                "--target", TRIPLE, "-o", output]
        if obj:
            args += ["--emit", "obj"]
        return self.command(args, env=self.env), output

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_elf_header_and_execution(self):
        result, output = self.build("fn main() {}", name="inspect")
        self.assert_ok(result)
        self.assert_ok(self.run_exe(output))
        data = output.read_bytes()
        self.assertEqual(data[:4], b"\x7fELF")
        self.assertEqual(data[4], 1)                      # ELFCLASS32
        self.assertIn(struct.unpack_from("<H", data, 16)[0], (2, 3))
        self.assertEqual(struct.unpack_from("<H", data, 18)[0], 40)  # EM_ARM

    def test_integer_arithmetic_matches_python_oracle(self):
        if CC is None:
            self.skipTest("arm cross compiler unavailable")
        operations = {"add": "+", "sub": "-", "mul": "*", "div": "/", "mod": "%",
                      "band": "&", "bor": "|", "bxor": "^", "shl": "<<", "shr": ">>",
                      "eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">="}
        comparisons = {"eq", "ne", "lt", "le", "gt", "ge"}
        declarations, functions = [], []
        for sign, reo_type, c_type in (("s", "i64", "int64_t"), ("u", "u64", "uint64_t")):
            for name, operator in operations.items():
                ret = "bool" if name in comparisons else reo_type
                cname = "bool" if name in comparisons else c_type
                functions.append(f"fn {sign}_{name}(a: {reo_type}, b: {reo_type}) -> {ret} {{ return a {operator} b; }}")
                declarations.append(f"extern {cname} {sign}_{name}({c_type}, {c_type});")
        native, obj = self.build("\n".join(functions), name="ops", obj=True)
        self.assert_ok(native)
        rng = random.Random(999983)
        pairs = [(0, 1), (1, 63), ((1 << 64) - 1, 2), (1 << 63, 1), (7, (1 << 64) - 1)]
        pairs += [(rng.getrandbits(64), rng.getrandbits(64) or 1) for _ in range(24)]
        calls, expected = [], []
        mask = (1 << 64) - 1
        for sign in ("s", "u"):
            for x, y in pairs:
                a = x - (1 << 64) if sign == "s" and x >= 1 << 63 else x
                b = y - (1 << 64) if sign == "s" and y >= 1 << 63 else y
                quotient = (abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1)
                values = {"add": a + b, "sub": a - b, "mul": a * b,
                          "div": quotient, "mod": a - quotient * b,
                          "band": a & b, "bor": a | b, "bxor": a ^ b,
                          "shl": a << (b & 63), "shr": a >> (b & 63),
                          "eq": a == b, "ne": a != b, "lt": a < b,
                          "le": a <= b, "gt": a > b, "ge": a >= b}
                for name in operations:
                    calls.append(f'printf("%llu\\n", (unsigned long long){sign}_{name}(UINT64_C({x}), UINT64_C({y})));')
                    expected.append(str(int(values[name]) & mask))
        harness = self.root / "harness.c"
        harness.write_text("#include <stdint.h>\n#include <stdbool.h>\n#include <stdio.h>\n" +
                           "\n".join(declarations) + "\nint main(void) {\n" + "\n".join(calls) + "\n}\n")
        exe = self.root / "oracle"
        self.assert_ok(self.command([CC, "-static", harness, obj, "-o", exe]))
        result = self.run_exe(exe)
        self.assert_ok(result)
        self.assertEqual(result.stdout.splitlines(), expected)

    def test_aapcs_argument_layout_interop(self):
        if CC is None:
            self.skipTest("arm cross compiler unavailable")
        source = '''
fn add6(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64) -> i64 { return a + b + c + d + e + f; }
fn mix(a: i32, b: i64, c: i32) -> i64 { return (a as i64) + b + (c as i64); }
fn narrow(x: i8, y: u16) -> i32 { return (x as i32) + (y as i32); }
extern { fn host_seven() -> i64; }
fn call_host() -> i64 { return host_seven() + 1; }
'''
        native, obj = self.build(source, name="abi", obj=True)
        self.assert_ok(native)
        c = self.root / "abi_main.c"
        c.write_text("#include <stdint.h>\n#include <stdio.h>\n"
                     "extern int64_t add6(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);\n"
                     "extern int64_t mix(int32_t,int64_t,int32_t);\n"
                     "extern int32_t narrow(int8_t,uint16_t);\n"
                     "extern int64_t call_host(void);\n"
                     "int64_t host_seven(void){ return 7; }\n"
                     "int main(void){ printf(\"%lld %lld %d %lld\\n\","
                     "(long long)add6(1,2,3,4,5,6),(long long)mix(-5,100,7),narrow(-1,65535),(long long)call_host());"
                     " return 0; }\n")
        exe = self.root / "abi"
        self.assert_ok(self.command([CC, "-static", c, obj, "-o", exe]))
        result = self.run_exe(exe)
        self.assert_ok(result)
        self.assertEqual(result.stdout.strip(), "21 102 65534 8")

    def test_runtime_failures_are_nonzero(self):
        for expression in ("assert(false)", "1 / 0", "1 % 0",
                           "(9223372036854775808u64 as i64) / -1"):
            with self.subTest(expression=expression):
                result, exe = self.build("fn main() { " + expression + "; }")
                self.assert_ok(result)
                self.assertNotEqual(self.run_exe(exe).returncode, 0)

    def test_floating_point_is_rejected_explicitly(self):
        result, output = self.build("fn main() { let x: f64 = 1.5; }")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("floating point", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
