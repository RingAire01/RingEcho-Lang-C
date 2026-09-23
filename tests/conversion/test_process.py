"""Exercise native process arguments and private build files without a network."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()


class ProcessTests(unittest.TestCase):
    def test_shared_library_build_has_a_terminated_argument_vector(self):
        with tempfile.TemporaryDirectory(prefix="reo-shared-") as directory:
            source = Path(directory) / "source.reo"
            source.write_text("fn value() -> i64 { return 42; }", encoding="utf-8")
            output = Path(directory) / ("library.dll" if os.name == "nt" else "library.so")
            result = subprocess.run([str(COMPILER), "build", str(source), "--shared", "-o", str(output)],
                                    cwd=directory, text=True, capture_output=True, timeout=30,
                                    env={**os.environ, "REO_KEEP_C": "0"})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.is_file())
            self.assertGreater(output.stat().st_size, 0)

    @unittest.skipIf(os.name == "nt", "POSIX executable fixture; Windows argument quoting needs native coverage")
    def test_wasm_output_is_passed_as_one_literal_argument(self):
        with tempfile.TemporaryDirectory(prefix="reo-arguments-") as directory:
            root = Path(directory)
            source = root / "source.reo"
            source.write_text('fn main() { println("hello"); }', encoding="utf-8")
            compiler = root / "fake compiler"
            compiler.write_text("#!/usr/bin/env python3\nimport json, os, pathlib, sys\n"
                                "pathlib.Path(os.environ['REO_ARGUMENT_LOG']).write_text(json.dumps(sys.argv[1:]))\n",
                                encoding="utf-8")
            compiler.chmod(0o700)
            output = root / 'output $(touch injected) "quoted".wasm'
            log = root / "arguments.json"
            result = subprocess.run([str(COMPILER), "build", str(source), "--target", "wasm", "-o", str(output)],
                                    cwd=root, text=True, capture_output=True, timeout=20,
                                    env={**os.environ, "REO_WASI_CC": str(compiler), "REO_ARGUMENT_LOG": str(log)})
            self.assertEqual(result.returncode, 0, result.stderr)
            arguments = json.loads(log.read_text())
            self.assertEqual(arguments[-2:], ["-o", str(output)])
            self.assertFalse((root / "injected").exists())
            generated = Path(arguments[2])
            self.assertTrue(generated.is_file())
            self.assertNotEqual(generated.parent, root)
            self.assertEqual(generated.parent.stat().st_mode & 0o777, 0o700)
            self.assertEqual(generated.stat().st_mode & 0o777, 0o600)
            generated.unlink()
            generated.parent.rmdir()


if __name__ == "__main__":
    unittest.main()
