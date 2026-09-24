"""Native ELF, execution, SysV ABI, differential and failure-path regressions."""
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
SUPPORTED = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")


@unittest.skipUnless(SUPPORTED, "native execution requires x86-64 Linux")
class NativeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reo native ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, args, **kwargs):
        return subprocess.run([str(a) for a in args], cwd=self.root,
                              text=True, capture_output=True, timeout=40, **kwargs)

    def build(self, source, name="program", obj=False, backend="native", env=None):
        path = self.root / (name + ".reo")
        path.write_text(source, encoding="utf-8")
        output = self.root / (name + (".o" if obj else ".exe"))
        args = [COMPILER, "build", path, "--backend", backend, "-o", output]
        if obj:
            args += ["--emit", "obj"]
        result = self.command(args, env=env)
        return result, output

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_executable_and_run_without_c_compiler(self):
        source = (ROOT / "tests/native/basic.reo").read_text(encoding="utf-8")
        result, output = self.build(source, env={**os.environ, "REO_CC": "/does/not/exist"})
        self.assert_ok(result)
        self.assert_ok(self.command([output]))
        data = output.read_bytes()
        self.assertEqual(data[:4], b"\x7fELF")
        self.assertEqual(struct.unpack_from("<HH", data, 16), (2, 62))
        phoff = struct.unpack_from("<Q", data, 32)[0]
        phsize, phcount = struct.unpack_from("<HH", data, 54)
        headers = [struct.unpack_from("<II", data, phoff + i * phsize) for i in range(phcount)]
        self.assertNotIn(3, [kind for kind, _ in headers])  # no PT_INTERP
        self.assertIn((0x6474E551, 6), headers)  # RW, non-executable GNU_STACK
        self.assert_ok(self.command([COMPILER, "run", self.root / "program.reo", "--backend", "native"]))

    def test_c_and_native_execute_the_same_control_flow(self):
        source = (ROOT / "tests/native/basic.reo").read_text(encoding="utf-8")
        for backend in ("c", "native"):
            with self.subTest(backend=backend):
                result, output = self.build(source, name=backend, backend=backend)
                self.assert_ok(result)
                self.assert_ok(self.command([output]))

    def test_object_is_deterministic_and_has_no_entry_stub(self):
        source = "fn answer() -> i64 { return 42; }"
        a, first = self.build(source, name="first", obj=True)
        b, second = self.build(source, name="second", obj=True)
        self.assert_ok(a)
        self.assert_ok(b)
        data = first.read_bytes()
        self.assertEqual(data, second.read_bytes())
        self.assertEqual(struct.unpack_from("<HH", data, 16), (1, 62))
        symbols = self.command(["nm", first])
        self.assert_ok(symbols)
        self.assertIn("answer", symbols.stdout)
        self.assertNotIn("_start", symbols.stdout)

    def test_arithmetic_matches_c_backend_and_python_oracle(self):
        operations = {"add": "+", "sub": "-", "mul": "*", "div": "/", "mod": "%",
                      "band": "&", "bor": "|", "bxor": "^", "shl": "<<", "shr": ">>",
                      "eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">="}
        comparisons = {"eq", "ne", "lt", "le", "gt", "ge"}
        declarations, functions = [], []
        for sign, reo_type, c_type in (("s", "i64", "int64_t"), ("u", "u64", "uint64_t")):
            for name, operator in operations.items():
                ret = "bool" if name in comparisons else reo_type
                functions.append(f"fn {sign}_{name}(a: {reo_type}, b: {reo_type}) -> {ret} {{ return a {operator} b; }}")
                declarations.append(f"extern {'bool' if ret == 'bool' else c_type} {sign}_{name}({c_type}, {c_type});")
        source = "\n".join(functions)
        native, obj = self.build(source, name="ops", obj=True)
        self.assert_ok(native)
        library = self.root / "ops.so"
        self.assert_ok(self.command([COMPILER, "build", self.root / "ops.reo", "--shared", "-o", library]))
        rng = random.Random(314159)
        pairs = [(0, 1), (1, 63), ((1 << 64) - 1, 2), (1 << 63, 1), (7, (1 << 64) - 1)]
        pairs += [(rng.getrandbits(64), rng.getrandbits(64) or 1) for _ in range(20)]
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
        for name, artifact in (("native", obj), ("c", library)):
            exe = self.root / (name + "-oracle")
            self.assert_ok(self.command(["gcc", harness, artifact, "-o", exe]))
            result = self.command([exe])
            self.assert_ok(result)
            self.assertEqual(result.stdout.splitlines(), expected, name)

    def test_external_abi_relocations_alignment_and_bool(self):
        source = '''
extern { fn alignment() -> i64; fn external_bool() -> bool; }
fn bool_identity(x: bool) -> bool { return x; }
fn six(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64) -> i64 {
    return a + b + c + d + e + f;
}
fn entry() -> i64 {
    assert(alignment() == 0);
    assert(1 + alignment() == 1);
    assert(external_bool());
    return six(1, 2 + alignment(), 3, 4 + alignment(), 5, 6);
}
'''
        result, obj = self.build(source, obj=True)
        self.assert_ok(result)
        asm = self.root / "helper.S"
        asm.write_text('''
.text
.globl alignment
alignment:
    leaq 8(%rsp), %rax
    andq $15, %rax
    ret
.globl external_bool
external_bool:
    movabsq $0xdeadbeef00000001, %rax
    ret
.globl bool_argument
bool_argument:
    movabsq $0xdeadbeef00000000, %rdi
    jmp bool_identity
.section .note.GNU-stack,"",@progbits
''')
        c = self.root / "helper.c"
        c.write_text("#include <stdint.h>\n#include <stdbool.h>\n"
                     "extern int64_t entry(void); extern bool bool_argument(void);\n"
                     "int main(void) { return entry() != 21 || bool_argument(); }\n")
        exe = self.root / "abi"
        self.assert_ok(self.command(["gcc", c, asm, obj, "-o", exe]))
        self.assert_ok(self.command([exe]))

    def test_unsupported_features_fail_without_replacing_output(self):
        cases = [
            'fn main() { println("hello"); }',
            'type Number = i64; fn main() {}',
            'fn main() { let a = [1, 2]; }',
            'fn f() -> i64 { let x = 1; } fn main() {}',
            'fn f(a:i64,b:i64,c:i64,d:i64,e:i64,f:i64,g:i64) {} fn main() {}',
            'fn f() -> i64 { return 1; }',
            'fn main() { let x: i128 = 1; }',
            'fn main() { let x = 1; let p = &x; }',
        ]
        output = self.root / "program.exe"
        for source in cases:
            with self.subTest(source=source):
                output.write_bytes(b"keep existing artifact")
                result, _ = self.build(source)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("native backend", result.stderr)
                self.assertEqual(output.read_bytes(), b"keep existing artifact")

    def test_linker_failure_preserves_output_and_cleans_temporary_files(self):
        output = self.root / "program.exe"
        output.write_bytes(b"previous")
        result, _ = self.build("fn main() {}", env={**os.environ, "REO_LD": "/bin/false"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native linker failed", result.stderr)
        self.assertEqual(output.read_bytes(), b"previous")
        self.assertEqual(list(self.root.glob("*.reo-*")), [])

    def test_unsigned_wrap_casts_unary_and_if_expression(self):
        source = '''
fn choose(flag: bool) -> i64 { return if flag { 11 } else { 22 }; }
fn main() {
    let max: u64 = 18446744073709551615u64;
    assert(max + 1u64 == 0u64);
    assert((max as i64) == -1);
    assert((0u64 as bool) == false);
    assert((max as bool) == true);
    assert(choose(true) == 11);
    assert(choose(false) == 22);
    assert(!false);
    assert(~0 == -1);
    assert(-(-8) == 8);
}
'''
        for backend in ("c", "native"):
            with self.subTest(backend=backend):
                result, exe = self.build(source, name=backend, backend=backend)
                self.assert_ok(result)
                self.assert_ok(self.command([exe]))

    def test_lowering_resource_limits_return_diagnostics(self):
        cases = [
            "fn main() {" + "".join(f"let x{i}: i64 = {i};" for i in range(1025)) + "}",
            "".join(f"fn f{i}() {{}}" for i in range(1025)),
        ]
        for source in cases:
            result, output = self.build(source, obj=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("limit exceeded", result.stderr)
            self.assertFalse(output.exists())

    def test_unresolved_extern_fails_link_and_missing_parent_fails_write(self):
        source = "extern { fn absent() -> i64; } fn main() { assert(absent() == 1); }"
        result, output = self.build(source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native linker failed", result.stderr)
        self.assertFalse(output.exists())
        path = self.root / "program.reo"
        path.write_text("fn main() {}")
        result = self.command([COMPILER, "build", path, "--backend", "native", "--emit", "obj",
                               "-o", self.root / "missing" / "out.o"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot create native output", result.stderr)

    def test_short_write_preserves_previous_output(self):
        import resource
        import signal

        def restrict_output_size():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (32, 32))

        path = self.root / "source.reo"
        path.write_text("fn main() {}")
        output = self.root / "previous.o"
        output.write_bytes(b"previous")
        result = self.command([COMPILER, "build", path, "--backend", "native",
                               "--emit", "obj", "-o", output], preexec_fn=restrict_output_size)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native output was not published", result.stderr)
        self.assertEqual(output.read_bytes(), b"previous")
        self.assertEqual(list(self.root.glob("*.reo-*")), [])

    def test_runtime_failures_are_nonzero(self):
        for expression in ("assert(false)", "1 / 0", "1 % 0",
                           "(9223372036854775808u64 as i64) / -1",
                           "(9223372036854775808u64 as i64) % -1"):
            with self.subTest(expression=expression):
                result, exe = self.build("fn main() { " + expression + "; }")
                self.assert_ok(result)
                self.assertNotEqual(self.command([exe]).returncode, 0)

    def test_cli_rejects_invalid_combinations(self):
        source = self.root / "main.reo"
        source.write_text("fn main() {}")
        for options in (["--backend", "unknown"], ["--backend", "native", "--shared"],
                        ["--backend", "native", "--target", "aarch64"],
                        ["--backend", "native", "--emit", "unknown"], ["--backend"],
                        ["--emit", "obj"], ["--target", "typo"]):
            with self.subTest(options=options):
                result = self.command([COMPILER, "build", source, *options])
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
