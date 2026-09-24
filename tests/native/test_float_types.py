"""SSE ABI and floating-point semantics compared against the C backend."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "x86_64", "x86-64 Linux")
class FloatTypeTests(unittest.TestCase):
    def command(self, args, root):
        result = subprocess.run([str(a) for a in args], cwd=root, text=True,
                                capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def differential(self, source, declarations, calls):
        with tempfile.TemporaryDirectory(prefix="reo-float-") as directory:
            root = Path(directory)
            path, obj, lib = root / "ops.reo", root / "native.o", root / "c.so"
            path.write_text(source)
            self.command([COMPILER, "build", path, "--backend", "native", "--emit", "obj", "-o", obj], root)
            self.command([COMPILER, "build", path, "--shared", "-o", lib], root)
            harness = root / "main.c"
            harness.write_text("#include <stdint.h>\n#include <stdbool.h>\n#include <stdio.h>\n#include <math.h>\n" +
                               "static void show(double x) { if (isnan(x)) puts(\"nan\"); else printf(\"%a\\n\", x); }\n" +
                               declarations + "\nint main(void) {\n" + calls + "\n}\n")
            outputs = []
            for name, artifact in (("native", obj), ("c", lib)):
                exe = root / name
                self.command(["gcc", harness, artifact, "-o", exe], root)
                outputs.append(self.command([exe], root))
            self.assertEqual(outputs[0].splitlines(), outputs[1].splitlines())

    def test_arithmetic_comparisons_nan_infinity_and_signed_zero(self):
        functions, declarations, calls = [], [], []
        pairs = [("1.25", "2.5"), ("-3.5", "0.25"), ("0.0", "-0.0"),
                 ("1.0", "0.0"), ("0.0", "0.0"), ("NAN", "1.0"),
                 ("1.0", "NAN"), ("INFINITY", "-INFINITY")]
        operations = {"add": "+", "sub": "-", "mul": "*", "div": "/",
                      "eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">="}
        for kind, ctype in (("f32", "float"), ("f64", "double")):
            for name, op in operations.items():
                comparison = name in ("eq", "ne", "lt", "le", "gt", "ge")
                ret = "bool" if comparison else kind
                functions.append(f"fn {kind}_{name}(a: {kind}, b: {kind}) -> {ret} {{ return a {op} b; }}")
                declarations.append(f"extern {'bool' if comparison else ctype} {kind}_{name}({ctype}, {ctype});")
                for a, b in pairs:
                    call = f"{kind}_{name}({a}, {b})"
                    calls.append(f'printf("%d\\n", {call});' if comparison else f"show({call});")
        self.differential("\n".join(functions), "\n".join(declarations), "\n".join(calls))

    def test_numeric_casts_match_saturation_and_rounding(self):
        types = [("i8", "int8_t"), ("i16", "int16_t"), ("i32", "int32_t"), ("i64", "int64_t"),
                 ("u8", "uint8_t"), ("u16", "uint16_t"), ("u32", "uint32_t"), ("u64", "uint64_t"),
                 ("char", "uint8_t"), ("bool", "bool")]
        values = ["NAN", "INFINITY", "-INFINITY", "0.0", "-0.0", "1.9", "-1.9", "300.0", "-300.0",
                  "0x1p63", "-0x1p63", "0x1p64", "0x1.fffffffffffffp63", "1e300", "-1e300"]
        functions, declarations, calls = [], [], []
        for src, csrc in (("f32", "float"), ("f64", "double")):
            for dst, cdst in types:
                name = f"{src}_to_{dst}"
                functions.append(f"fn {name}(a: {src}) -> {dst} {{ return a as {dst}; }}")
                declarations.append(f"extern {cdst} {name}({csrc});")
                for value in values:
                    calls.append(f'printf("%llu\\n", (unsigned long long){name}({value}));')
        for src, csrc in (("i64", "int64_t"), ("u64", "uint64_t")):
            for dst, cdst in (("f32", "float"), ("f64", "double")):
                name = f"{src}_to_{dst}"
                functions.append(f"fn {name}(a: {src}) -> {dst} {{ return a as {dst}; }}")
                declarations.append(f"extern {cdst} {name}({csrc});")
                for value in (0, 1, (1 << 63) - 1, 1 << 63, (1 << 64) - 1, (1 << 54) + 3):
                    calls.append(f"show({name}(UINT64_C({value})));")
        self.differential("\n".join(functions), "\n".join(declarations), "\n".join(calls))

    def test_mixed_register_calls_and_float_locals(self):
        source = '''
fn mix(a: i64, b: f64, c: i64, d: f32, e: f64, f: i64) -> f64 {
    let value: f64 = b + e;
    value += d as f64;
    return value + ((a + c + f) as f64);
}
fn run() -> f64 { return 1.0 + mix(1, 2.5, 3, 4.5f32, 5.5, 6); }
fn neg(a: f32) -> f32 { return -a; }
fn narrow(a: f64) -> f32 { return a as f32; }
fn widen(a: f32) -> f64 { return a as f64; }
'''
        declarations = "extern double run(void); extern float neg(float); extern float narrow(double); extern double widen(float);"
        calls = "show(run()); show(neg(0.0)); show(neg(-0.0)); show(narrow(1.0/3.0)); show(widen(0.1f));"
        self.differential(source, declarations, calls)


if __name__ == "__main__":
    unittest.main()
