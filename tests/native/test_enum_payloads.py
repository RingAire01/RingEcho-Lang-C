"""Typed enum payload metadata and C-backend representation regressions."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()


class EnumPayloadTests(unittest.TestCase):
    def compile(self, source, command="run"):
        with tempfile.TemporaryDirectory(prefix="reo-enum-") as directory:
            path = Path(directory) / "main.reo"
            path.write_text(source)
            return subprocess.run([str(COMPILER), command, str(path)], cwd=directory,
                                  text=True, capture_output=True, timeout=30)

    def test_payloads_preserve_actual_types_and_all_arguments(self):
        result = self.compile('''
struct Item { number: i64 }
enum Message { Empty, Wide(i128), Real(f64), Pair(u8, u64), Text(str), Object(Item) }
fn main() {
    let wide = Message::Wide(18446744073709551617i128);
    assert(wide.tag == 1);
    assert(wide.u.v1 == 18446744073709551617i128);
    let real = Message::Real(1.25);
    assert(real.u.v2 == 1.25);
    let pair = Message::Pair(7u8, 18446744073709551615u64);
    assert(pair.u.v3.f0 == 7u8);
    assert(pair.u.v3.f1 == 18446744073709551615u64);
    let text = Message::Text("typed payload");
    assert(text.u.v4 == "typed payload");
    let object = Message::Object(Item { number: 42 });
    assert(object.u.v5.number == 42);
}
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_payload_arity_and_type_errors_are_rejected(self):
        for value, diagnostic in (("Choice::Pair(1u8)", "expects 2 payload arguments"),
                                  ("Choice::Pair(1u8, 2u64, 3)", "expects 2 payload arguments"),
                                  ('Choice::Pair("bad", 2u64)', "enum payload type mismatch")):
            with self.subTest(value=value):
                result = self.compile("enum Choice { Pair(u8,u64) } fn main() { let x = " + value + "; }", "check")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)

    def test_complex_payload_type_syntax_is_parsed_as_types(self):
        result = self.compile("enum Data { Array([u8;4]), Reference(&u64), Function(fn(i64)->i64), Tuple((u8,u64)) } fn main() {}", "check")
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
