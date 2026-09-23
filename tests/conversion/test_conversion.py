"""Assert conversion results and diagnostics in isolated temporary directories."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()


class ConversionTests(unittest.TestCase):
    def execute(self, source, command="run", expected=None, diagnostic=None, environment=None):
        with tempfile.TemporaryDirectory(prefix="reo-conversion-") as directory:
            path = Path(directory) / "case.reo"
            path.write_text(source, encoding="utf-8")
            arguments = [command] if isinstance(command, str) else command
            result = subprocess.run([str(COMPILER), arguments[0], str(path), *arguments[1:]], cwd=directory,
                                    text=True, capture_output=True, timeout=20,
                                    env={**os.environ, "REO_KEEP_C": "0", **(environment or {})})
            if diagnostic:
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(diagnostic, result.stderr)
            else:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("runtime error:", result.stderr)
                if expected is not None:
                    self.assertEqual(result.stdout, expected)
            return result

    def test_scalar_conversion_results(self):
        self.execute('''fn main() {
            println(256 as bool); println(0 as bool);
            println(true as str); println('A' as str);
            println("abc" as str); println("true" as bool);
            println("Z" as char); println(300.5 as i8);
            println((-3.5) as u64);
            println((("16777217" as f32) as f64) as i64);
            println((300.5 as char) as i64);
        }''', expected="1\n0\ntrue\nA\nabc\n1\n90\n127\n0\n16777216\n255\n")

    def test_wide_integer_boundaries_and_formatting(self):
        self.execute('''fn main() {
            println(340282366920938463463374607431768211455u128);
            println(-170141183460469231731687303715884105728i128);
            println(18446744073709551615u64);
            println((1000000000000000000000000u128) as str);
            println("340282366920938463463374607431768211455" as u128);
            println("-170141183460469231731687303715884105728" as i128);
            println((1u128 << 100) as str);
            println((340282366920938463463374607431768211455u128 / 3u128) as str);
        }''', expected=f"{2**128-1}\n{-2**127}\n{2**64-1}\n{10**24}\n{2**128-1}\n{-2**127}\n{2**100}\n{(2**128-1)//3}\n")

    def test_float_upper_bound_is_saturated(self):
        self.execute('''fn main() {
            println(("9223372036854775808" as f64) as i64);
            println(("18446744073709551616" as f64) as u64);
            println(("100000000000000000000" as f64) as i128);
        }''', expected=f"{2**63-1}\n{2**64-1}\n{10**20}\n")

    def test_alias_and_builtin_argument_validation(self):
        self.execute('type Small = u8\nfn main() { println(300 as Small); }', expected="44\n")
        self.execute('struct P { x: i64 }\nfn main() { println(P { x: 1 } as i64); }',
                     command="check", diagnostic="invalid cast")

    def test_strict_parsing_rejects_invalid_input(self):
        for text in ("", "garbage", "12xyz", " 1", "18446744073709551616"):
            with self.subTest(text=text):
                self.execute(f'fn main() {{ println("{text}" as u64); }}',
                             diagnostic="conversion failed")

    def test_literal_overflow_is_rejected(self):
        self.execute('fn main() { println(340282366920938463463374607431768211456u128); }',
                     command="check", diagnostic="exceeds 128 bits")
        self.execute('fn main() { println(256u8); }', command="check", diagnostic="outside the range")


    def test_checked_results_preserve_payload_and_propagate_errors(self):
        self.execute('fn parse(s: str) -> Result<u128, ConversionError> {\n            let value = try_convert<u64>(s)?;\n            return try_convert<u128>(value);\n        }\n        fn main() {\n            let a = try_convert<u128>("340282366920938463463374607431768211455");\n            println(a.tag); println(a.value);\n            let b = try_convert<u8>("999"); println(b.tag); println(b.error);\n            let c = parse("18446744073709551615"); println(c.value);\n            let d = parse("bad"); println(d.tag);\n            let e = try_convert<f64>(9007199254740993u64); println(e.tag);\n            let f = try_convert<str>(0 as char); println(f.tag);\n        }', expected=f"0\n{2**128-1}\n1\n2\n{2**64-1}\n1\n1\n1\n")

    def test_lossy_implicit_conversion_is_rejected(self):
        for body in ("let x: i8 = 300;", "let x: i8 = 200.5;",
                     "let x = 300; let y: i8 = x;"):
            self.execute("fn main() { " + body + " }", command="check", diagnostic="type mismatch")

    def test_integer_matrix_matches_modular_reference(self):
        types = [(prefix + str(bits), bits, prefix == "i")
                 for prefix in ("i", "u") for bits in (8, 16, 32, 64, 128)]
        for source, width, signed in types:
            values = [0, 1, (1 << (width - int(signed))) - 1]
            if signed:
                values.extend([-1, -(1 << (width - 1))])
            body, expected = [], []
            for value in values:
                for target, bits, is_signed in types:
                    body.append(f"println(({value}{source}) as {target});")
                    converted = value % (1 << bits)
                    if is_signed and converted >= 1 << (bits - 1):
                        converted -= 1 << bits
                    expected.append(str(converted))
            with self.subTest(source=source):
                self.execute("fn main() { " + " ".join(body) + " }",
                             expected="\n".join(expected) + "\n")


    def test_wide_array_storage_and_checked_element_conversion(self):
        self.execute('fn main() {\n            let a = [1u128, 340282366920938463463374607431768211455u128];\n            let good = try_convert<[u128; 2]>(a);\n            println(good.tag); println(good.value[1]);\n            let bad = try_convert<[u64; 2]>(a);\n            println(bad.tag); println(bad.error); println(bad.index);\n            let small = try_convert<[i8; 2]>([1, 2]); println(small.value[1]);\n            let length = try_convert<[u64; 3]>(a); println(length.tag);\n            a[0] = 100000000000000000000u128;\n            println(a[0]); println(a[1]); println(len(a));\n            let signed_values = [-170141183460469231731687303715884105728i128; 2];\n            for value in signed_values { println(value); }\n        }', expected=f"0\n{2**128-1}\n1\n2\n1\n2\n1\n{10**20}\n{2**128-1}\n2\n{-2**127}\n{-2**127}\n")

    def test_nested_invalid_conversions_are_checked(self):
        for body in ("println(len([1, (true as ptr)]));",
                     "println(if true { 1 } else { true as ptr });"):
            self.execute("fn main() { " + body + " }", command="check", diagnostic="invalid cast")

    def test_radix_literals_alias_chains_and_exact_float_text(self):
        self.execute('type A = u128\ntype B = A\nfn main() {\n    println(0xffffffffffffffffffffffffffffffffu128);\n    println(- 170141183460469231731687303715884105728i128);\n    println(0b11111111u8); println(0o377u8);\n    println(123456789012345678901234567890 as B);\n    println((1.23456789012345 as str) as f64);\n    println(1e20 as i128);\n}', expected=f"{2**128-1}\n{-2**127}\n255\n255\n123456789012345678901234567890\n1.23457\n{10**20}\n")


    def test_checked_conversion_reports_allocation_failure(self):
        self.execute('fn main() { let a = try_convert<str>(42); println(a.tag); println(a.error); }',
                     expected="1\n5\n", environment={"REO_CC_OPT": "-D__REO_CONV_ALLOC(size)=((void*)0)"})

    def test_array_error_position_survives_scalar_result_propagation(self):
        self.execute('fn convert() -> Result<u64, ConversionError> { let a = try_convert<[u8; 2]>([1, 999])?; return try_convert<u64>(a[0]); } fn main() { let r = convert(); println(r.tag); println(r.error); println(r.index); let s = try_convert<u64>("bad"); println(s.index); }',
                     expected="1\n2\n1\n-1\n")

    def test_reo_backend_rejects_conversion_instead_of_emitting_zero(self):
        self.execute('fn main() { println(42 as i8); }', command=["build", "--target", "reo"],
                     diagnostic="backend does not support this conversion")

    def test_temporary_array_indexing_preserves_element_type(self):
        self.execute('fn main() { println([1u128, 2u128][1]); println([1.5, 2.5][0]); println([7, 8][0]); println(try_convert<[u128; 2]>([9, 10]).value[1]); }',
                     expected="2\n1.5\n7\n10\n")

    def test_field_and_element_assignments_reject_implicit_narrowing(self):
        for source in ("struct P { x: i8 } fn main() { let p = P { x: 1 }; p.x = 300; }",
                       "fn main() { let a = [1i8, 2i8]; a[0] = 300; }"):
            self.execute(source, command="check", diagnostic="assignment type mismatch")


if __name__ == "__main__":
    unittest.main()
