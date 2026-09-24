"""Typed C-backend reference emission; no pointer-to-integer local round trip."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()


class CPointerTests(unittest.TestCase):
    def test_typed_references_through_local_and_function_parameters(self):
        source = '''
fn read(p: &i64) -> i64 { return *p; }
fn next(p: &mut i64) -> i64 { return *p + 1; }
fn read_byte(p: &u8) -> u8 { return *p; }
fn main() {
    let value: i64 = 40;
    let byte: u8 = 255u8;
    let p: &i64 = &value;
    let q: &mut i64 = &mut value;
    let r = &byte;
    assert(read(p) == 40);
    assert(next(q) == 41);
    assert(read_byte(r) == 255u8);
}
'''
        with tempfile.TemporaryDirectory(prefix="reo-typed-pointer-") as directory:
            root = Path(directory)
            path = root / "pointer.reo"
            path.write_text(source)
            result = subprocess.run([str(COMPILER), "run", str(path)], cwd=root,
                                    capture_output=True, text=True, timeout=30,
                                    env={**os.environ, "REO_KEEP_C": "1"})
            self.assertEqual(result.returncode, 0, result.stderr)
            generated = root / "generated.c"
            result = subprocess.run([str(COMPILER), "build", str(path), "--target", "c-freestanding",
                                     "-o", str(generated)], cwd=root, capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            c_code = generated.read_text()
            self.assertIn("int64_t const* p", c_code)
            self.assertIn("int64_t* q", c_code)
            self.assertIn("uint8_t const* r", c_code)
            self.assertNotIn("(int64_t)(uintptr_t)&value", c_code)
            self.assertNotIn("(int64_t)(uintptr_t)&byte", c_code)


if __name__ == "__main__":
    unittest.main()
