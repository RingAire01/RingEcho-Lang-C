"""Exercise the installed tools without requiring a target C compiler."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def smoke(bin_dir):
    suffix = ".exe" if os.name == "nt" else ""
    bin_dir = bin_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="ringecho-installed-") as directory:
        source = Path(directory) / "smoke.reo"
        source.write_text('fn main() { println(42 as u8); }\n', encoding="utf-8")
        commands = [("rem", "version"), ("rvm", "version"), ("rev", "check", str(source))]
        for name, *arguments in commands:
            result = subprocess.run([str(bin_dir / (name + suffix)), *arguments], cwd=directory,
                                    capture_output=True, text=True, timeout=30,
                                    env={**os.environ, "REO_KEEP_C": "0"})
            if result.returncode:
                raise RuntimeError(f"{name}: {result.returncode}\n{result.stdout}\n{result.stderr}")
        source.write_text('fn main() { let value: u8 = 999; }\n', encoding="utf-8")
        result = subprocess.run([str(bin_dir / ("rev" + suffix)), "check", str(source)],
                                cwd=directory, capture_output=True, text=True, timeout=30)
        if result.returncode == 0 or "type mismatch" not in result.stderr:
            raise RuntimeError(f"缺少预期的类型诊断：{result.stdout}\n{result.stderr}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("bin_dir", type=Path)
    smoke(parser.parse_args().bin_dir)
