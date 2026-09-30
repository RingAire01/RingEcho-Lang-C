"""AArch64 native backend: ELF64 output, integer arithmetic, AAPCS64 ABI.

These tests run on a native aarch64 Linux host (for example the
ubuntu-24.04-arm CI runner), where the generated executables run directly and
the relocatable objects link against a native C compiler to check the AAPCS64
calling convention.
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
TRIPLE = "aarch64-unknown-linux-gnu"
SUPPORTED = platform.system() == "Linux" and platform.machine() in ("aarch64", "arm64")


@unittest.skipUnless(SUPPORTED, "AArch64 execution requires an aarch64 Linux host")
class NativeAArch64Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reo native aarch64 ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, args, **kwargs):
        return subprocess.run([str(a) for a in args], cwd=self.root,
                              text=True, capture_output=True, timeout=40, **kwargs)

    def build(self, source, name="program", obj=False):
        path = self.root / (name + ".reo")
        path.write_text(source, encoding="utf-8")
        output = self.root / (name + (".o" if obj else ".elf"))
        args = [COMPILER, "build", path, "--backend", "native",
                "--target", TRIPLE, "-o", output]
        if obj:
            args += ["--emit", "obj"]
        return self.command(args), output

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def run_linked(self, exe):
        """Run a C-linked executable; on a signal, include a gdb backtrace."""
        result = self.command([exe])
        if result.returncode < 0:
            diag = ""
            try:
                gdb = subprocess.run(
                    ["gdb", "-batch", "-ex", "run",
                     "-ex", "print $_siginfo",
                     "-ex", "info registers pc sp x29 x0 x1 x2",
                     "-ex", "x/4i $pc", "-ex", "bt", "--args", str(exe)],
                    cwd=self.root, text=True, capture_output=True, timeout=60)
                diag = "\n--- gdb ---\n" + gdb.stdout + gdb.stderr
            except (OSError, subprocess.SubprocessError) as exc:
                diag = "\ngdb unavailable: %s" % exc
            self.fail("linked executable died with signal %d%s" % (-result.returncode, diag))
        self.assert_ok(result)
        return result

    def test_elf_header_and_execution(self):
        result, output = self.build("fn main() {}", name="inspect")
        self.assert_ok(result)
        self.assert_ok(self.command([output]))
        data = output.read_bytes()
        self.assertEqual(data[:4], b"\x7fELF")
        self.assertEqual(data[4], 2)                      # ELFCLASS64
        self.assertIn(struct.unpack_from("<H", data, 16)[0], (2, 3))
        self.assertEqual(struct.unpack_from("<H", data, 18)[0], 183)  # EM_AARCH64

    def test_object_is_deterministic(self):
        source = "fn answer() -> i64 { return 42; }"
        a, first = self.build(source, name="a", obj=True)
        b, second = self.build(source, name="b", obj=True)
        self.assert_ok(a); self.assert_ok(b)
        self.assertEqual(first.read_bytes(), second.read_bytes())

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
        native, obj = self.build("\n".join(functions), name="ops", obj=True)
        self.assert_ok(native)
        rng = random.Random(141421)
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
        self.assert_ok(self.command(["gcc", harness, obj, "-o", exe]))
        result = self.run_linked(exe)
        self.assertEqual(result.stdout.splitlines(), expected)

    def test_c_abi_argument_layout_interop(self):
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
        self.assert_ok(self.command(["gcc", c, obj, "-o", exe]))
        result = self.run_linked(exe)
        self.assertEqual(result.stdout.strip(), "21 102 65534 8")

    def test_runtime_failures_are_nonzero(self):
        for expression in ("assert(false)", "1 / 0", "1 % 0",
                           "(9223372036854775808u64 as i64) / -1"):
            with self.subTest(expression=expression):
                result, exe = self.build("fn main() { " + expression + "; }")
                self.assert_ok(result)
                self.assertNotEqual(self.command([exe]).returncode, 0)

    def test_floating_point_is_rejected_explicitly(self):
        result, output = self.build("fn main() { let x: f64 = 1.5; }")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("floating point", result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
