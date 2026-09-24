"""Machine execution and C ABI checks for all native integer widths <= 64."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "x86_64", "x86-64 Linux ABI")
class IntegerTypeTests(unittest.TestCase):
    def run_command(self, args, cwd):
        result = subprocess.run([str(a) for a in args], cwd=cwd, text=True,
                                capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_integer_width_operations_and_abi_match_c(self):
        kinds = [("i8", "int8_t", -128, 127), ("i16", "int16_t", -32768, 32767),
                 ("i32", "int32_t", -(1 << 31), (1 << 31) - 1),
                 ("u8", "uint8_t", 0, 255), ("u16", "uint16_t", 0, 65535),
                 ("u32", "uint32_t", 0, (1 << 32) - 1),
                 ("isize", "intptr_t", -(1 << 63), (1 << 63) - 1),
                 ("usize", "uintptr_t", 0, (1 << 64) - 1)]
        functions, declarations, calls = [], [], []
        operations = {"add": "+", "sub": "-", "mul": "*", "shr": ">>", "shl": "<<", "div": "/", "mod": "%", "lt": "<"}
        for reo, c, minimum, maximum in kinds:
            for name, op in operations.items():
                result = "bool" if name == "lt" else reo
                cname = "bool" if name == "lt" else c
                functions.append(f"fn {reo}_{name}(a: {reo}, b: {reo}) -> {result} {{ return a {op} b; }}")
                declarations.append(f"extern {cname} {reo}_{name}({c}, {c});")
                for a, b in ((maximum, 2), (minimum, 2), (7, 3), (maximum, 15)):
                    calls.append(f'printf("%llu\\n", (unsigned long long){reo}_{name}(({c})UINT64_C({a & ((1 << 64) - 1)}), ({c}){b}));')
        functions.append("fn echo_char(c: char) -> char { return c; }")
        declarations.append("extern uint8_t echo_char(uint8_t);")
        calls.append('printf("%u\\n", (unsigned)echo_char(255));')
        with tempfile.TemporaryDirectory(prefix="reo-integer-") as directory:
            root = Path(directory)
            source = root / "types.reo"
            source.write_text("\n".join(functions))
            obj, lib = root / "native.o", root / "c.so"
            self.run_command([COMPILER, "build", source, "--backend", "native", "--emit", "obj", "-o", obj], root)
            self.run_command([COMPILER, "build", source, "--shared", "-o", lib], root)
            harness = root / "main.c"
            harness.write_text("#include <stdint.h>\n#include <stdio.h>\n#include <stdbool.h>\n" +
                               "\n".join(declarations) + "\nint main(void) {\n" + "\n".join(calls) + "\n}\n")
            outputs = []
            for name, artifact in (("native", obj), ("c", lib)):
                exe = root / name
                self.run_command(["gcc", harness, artifact, "-o", exe], root)
                outputs.append(self.run_command([exe], root).stdout)
            self.assertEqual(outputs[0], outputs[1])

    def test_narrow_casts_character_literals_and_compound_assignments(self):
        source = '''
fn main() {
    let n: i8 = 127i8;
    n += 1i8;
    assert(n == -128i8);
    assert((300 as u8) == 44u8);
    assert((-1 as u16) == 65535u16);
    assert((4294967295u64 as i32) == -1i32);
    assert((255u8 as i64) == 255);
    assert(('A' as u8) == 65u8);
    assert((66u8 as char) == 'B');
    assert((0u8 as bool) == false);
    assert((128u8 as bool) == true);
    let p: usize = 2usize;
    assert(p * 3usize == 6usize);
}
'''
        with tempfile.TemporaryDirectory(prefix="reo-narrow-") as directory:
            root = Path(directory)
            path = root / "main.reo"
            path.write_text(source)
            for backend in ("native", "c"):
                exe = root / backend
                self.run_command([COMPILER, "build", path, "--backend", backend, "-o", exe], root)
                self.run_command([exe], root)

    def test_narrow_signed_division_overflow_traps(self):
        with tempfile.TemporaryDirectory(prefix="reo-overflow-") as directory:
            root = Path(directory)
            for kind, minimum in (("i8", -128), ("i16", -32768), ("i32", -(1 << 31))):
                for op in ("/", "%"):
                    path = root / "main.reo"
                    path.write_text(f"fn main() {{ {minimum}{kind} {op} -1{kind}; }}")
                    exe = root / "program"
                    self.run_command([COMPILER, "build", path, "--backend", "native", "-o", exe], root)
                    result = subprocess.run([str(exe)], cwd=root, capture_output=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
