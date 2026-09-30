"""Mach-O native backend on macOS: object format, codegen and ABI.

Runs only on a macOS host. The relocatable objects are linked against a small
C harness with the system compiler and executed, which verifies the generated
machine code and calling convention on real macOS hardware. Freestanding
executable output is not exercised here.
"""
import os
from pathlib import Path
import platform
import random
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


@unittest.skipUnless(SUPPORTED, "Mach-O tests require a macOS host")
class NativeMacOSTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reo native macho ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, args, **kwargs):
        return subprocess.run([str(a) for a in args], cwd=self.root,
                              text=True, capture_output=True, timeout=60, **kwargs)

    def build(self, source, name="program", obj=True):
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

    def link(self, harness_c, obj, exe):
        return self.command(["cc", harness_c, obj, "-o", exe])

    def test_object_header_and_determinism(self):
        source = "fn answer() -> i64 { return 42; }"
        result, obj = self.build(source)
        self.assert_ok(result)
        data = obj.read_bytes()
        self.assertEqual(struct.unpack_from("<I", data, 0)[0], 0xFEEDFACF)
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], CPUTYPE)
        _, obj2 = self.build(source, name="again")
        self.assertEqual(obj.read_bytes(), obj2.read_bytes())

    def test_integer_arithmetic_matches_python_oracle(self):
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
        native, obj = self.build("\n".join(functions), name="ops")
        self.assert_ok(native)
        rng = random.Random(31415)
        pairs = [(0, 1), (1, 63), ((1 << 64) - 1, 2), (1 << 63, 1), (7, (1 << 64) - 1)]
        pairs += [(rng.getrandbits(64), rng.getrandbits(64) or 1) for _ in range(24)]
        calls, expected = [], []
        mask = (1 << 64) - 1
        for sign in ("s", "u"):
            for x, y in pairs:
                a = x - (1 << 64) if sign == "s" and x >= 1 << 63 else x
                b = y - (1 << 64) if sign == "s" and y >= 1 << 63 else y
                q = (abs(a) // abs(b)) * (-1 if (a < 0) != (b < 0) else 1)
                values = {"add": a + b, "sub": a - b, "mul": a * b, "div": q, "mod": a - q * b,
                          "band": a & b, "bor": a | b, "bxor": a ^ b,
                          "shl": a << (b & 63), "shr": a >> (b & 63),
                          "eq": a == b, "ne": a != b, "lt": a < b, "le": a <= b,
                          "gt": a > b, "ge": a >= b}
                for name in operations:
                    calls.append(f'printf("%llu\\n", (unsigned long long){sign}_{name}(UINT64_C({x}), UINT64_C({y})));')
                    expected.append(str(int(values[name]) & mask))
        harness = self.root / "harness.c"
        harness.write_text("#include <stdint.h>\n#include <stdbool.h>\n#include <stdio.h>\n" +
                           "\n".join(declarations) + "\nint main(void) {\n" + "\n".join(calls) + "\n}\n")
        exe = self.root / "oracle"
        self.assert_ok(self.link(harness, obj, exe))
        result = self.command([exe])
        self.assert_ok(result)
        self.assertEqual(result.stdout.splitlines(), expected)

    def test_abi_interop(self):
        source = '''
fn add6(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64) -> i64 { return a + b + c + d + e + f; }
fn mix(a: i32, b: i64, c: i32) -> i64 { return (a as i64) + b + (c as i64); }
fn narrow(x: i8, y: u16) -> i32 { return (x as i32) + (y as i32); }
extern { fn host_seven() -> i64; }
fn call_host() -> i64 { return host_seven() + 1; }
'''
        native, obj = self.build(source, name="abi")
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
        self.assert_ok(self.link(c, obj, exe))
        result = self.command([exe])
        self.assert_ok(result)
        self.assertEqual(result.stdout.strip(), "21 102 65534 8")

    def test_division_faults_are_nonzero(self):
        source = "fn div(a: i64, b: i64) -> i64 { return a / b; }"
        native, obj = self.build(source, name="dv")
        self.assert_ok(native)
        c = self.root / "fault.c"
        c.write_text("#include <stdint.h>\nint64_t div(int64_t,int64_t);\n"
                     "int main(void){ volatile int64_t x = div(1, 0); (void)x; return 0; }\n")
        exe = self.root / "fault"
        self.assert_ok(self.link(c, obj, exe))
        self.assertNotEqual(self.command([exe]).returncode, 0)

    @unittest.skipIf(MACHINE == "x86_64", "x86-64 supports floating point")
    def test_floating_point_is_rejected_explicitly(self):
        result, output = self.build("fn main() { let x: f64 = 1.5; }", obj=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("floating point", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
