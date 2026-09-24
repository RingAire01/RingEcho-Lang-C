"""End-to-end value matching and semantic diagnostics."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()


class MatchTests(unittest.TestCase):
    def compile_source(self, source, command="run"):
        with tempfile.TemporaryDirectory(prefix="reo-match-") as directory:
            path = Path(directory) / "main.reo"
            path.write_text(source, encoding="utf-8")
            return subprocess.run([str(COMPILER), command, str(path)], cwd=directory,
                                  text=True, capture_output=True, timeout=30)

    def test_string_matching_uses_content_and_returns_strings(self):
        result = self.compile_source('''
fn label(s: str) -> str {
    return match s { "hello" => "matched", _ => "other", };
}
fn main() {
    println(label(str_concat("hel", "lo")));
    println(label("absent"));
}
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["matched", "other"])

    def test_float_patterns_and_results_preserve_fraction(self):
        result = self.compile_source('''
fn classify(n: f64) -> f64 {
    return match n { 1.25 => 2.5, 1.75 => 3.5, _ => 4.5, };
}
fn main() {
    println(classify(1.25) == 2.5);
    println(classify(1.75) == 3.5);
    println(classify(9.0) == 4.5);
}
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["1"] * 3)

    def test_invalid_matches_fail_during_check(self):
        cases = [
            ('match 1 { "one" => 1, _ => 0, }', "match pattern type"),
            ('match 1 { 1 => 2, _ => "other", }', "match arm type"),
            ('match 1 { _ => 2, 1 => 3, }', "wildcard must be the last"),
            ('match 1 { _ => 2, _ => 3, }', "wildcard must be the last"),
            ('match 1u8 { 256 => 2, _ => 0, }', "match pattern type"),
            ('match 1 { 1 => 1u8, _ => 256, }', "match arm type"),
        ]
        for expression, diagnostic in cases:
            with self.subTest(expression=expression):
                result = self.compile_source(
                    "fn main() { let value = " + expression + "; println(value); }", "check")
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertIn(diagnostic, result.stderr)

    def test_char_bool_and_narrow_float_results(self):
        result = self.compile_source('''
fn main() {
    println((match 'a' { 'a' => 'z', _ => 'x', }) == 'z');
    println(match false { true => false, false => true, });
    let small: f32 = 1.5f32;
    println((match small { 1.5f32 => 2.5f32, _ => 0.0f32, }) == 2.5f32);
    println(match -2 { -2 => 5, _ => 0, });
}
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["1", "1", "1", "5"])

    def test_wide_values_and_nested_matches_preserve_type(self):
        result = self.compile_source('''
fn main() {
    let wide: u128 = 18446744073709551617u128;
    let selected = match wide { 1u128 => 0u128, _ => wide, };
    println(selected == wide);
    println(match wide { 1u128 => 0, 18446744073709551617u128 => 1, _ => 0, });
    println(match true { true => match "x" { "x" => "nested", _ => "bad", }, _ => "bad", });
}
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["1", "1", "nested"])

    def test_subject_is_evaluated_once_and_only_selected_arm_runs(self):
        result = self.compile_source('''
fn subject() -> i64 { println("subject"); return 2; }
fn arm(n: i64) -> i64 { println(n); return n; }
fn main() {
    println(match subject() { 1 => arm(10), 2 => arm(20), _ => arm(30), });
}
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["subject", "20", "20"])

    def test_unmatched_values_keep_zero_or_empty_string_defaults(self):
        result = self.compile_source('''
fn main() {
    println(match 2 { 1 => 9, });
    println(str_len(match 2 { 1 => "text", }));
    println(match 2 { _ => "wildcard", });
}
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["0", "0", "wildcard"])


if __name__ == "__main__":
    unittest.main()
