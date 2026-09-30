"""C <-> RingEcho interoperability for `extern "C"` foreign declarations.

Builds a small C fixture, links it against a Reo program that declares the
functions with `extern "C"`, and runs it. Also exports a Reo function from a
shared library and calls it from C. Skipped off Linux x86-64.
"""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()
SUPPORTED = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")

FIXTURE_C = r'''
#include <stdint.h>
int32_t ffi_add(int32_t a, int32_t b) { return a + b; }
int64_t ffi_mul(int64_t a, int64_t b) { return a * b; }
int32_t ffi_sum3(int32_t a, int32_t b, int32_t c) { return a + b + c; }
void   *ffi_ptr_roundtrip(void *p) { return p; }
int64_t ffi_mix(int32_t a, int64_t b, int32_t c) { return (int64_t)a + b + c; }
'''

FOREIGN_REO = r'''
extern "C" fn ffi_add(a: i32, b: i32) -> i32;
extern "C" fn ffi_mul(a: i64, b: i64) -> i64;
extern "C" fn ffi_sum3(a: i32, b: i32, c: i32) -> i32;
extern "C" fn ffi_ptr_roundtrip(p: ptr) -> ptr;
extern "C" fn ffi_mix(a: i32, b: i64, c: i32) -> i64;

fn main() {
    assert(ffi_add(20, 22) == 42);
    assert(ffi_mul(1000000, 1000000) == 1000000000000);
    assert(ffi_sum3(1, 2, 3) == 6);
    assert(ffi_mix(-5, 100, 7) == 102);
    let n: usize = 4096;
    let p: ptr = ffi_ptr_roundtrip(n as ptr);
    assert((p as usize) == 4096);
}
'''

EXPORT_REO = r'''
fn reo_add(a: i32, b: i32) -> i32 { return a + b; }
'''

EXPORT_HARNESS = r'''
#include <stdint.h>
extern int32_t reo_add(int32_t, int32_t);
int main(void) { return reo_add(3, 4) == 7 ? 0 : 1; }
'''


@unittest.skipUnless(SUPPORTED, "FFI interop test requires Linux x86-64")
class FfiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="reo-ffi-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def command(self, args, **kwargs):
        return subprocess.run([str(a) for a in args], cwd=self.root, text=True,
                              capture_output=True, timeout=60,
                              env={**os.environ, "REO_CC": "gcc", "REO_KEEP_C": "0"}, **kwargs)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reo_calls_c(self):
        fixture = self.root / "fixture.c"
        fixture.write_text(FIXTURE_C)
        self.assert_ok(self.command(["gcc", "-c", fixture, "-o", "fixture.o"]))
        self.assert_ok(self.command(["ar", "rcs", "libreo_ffi.a", "fixture.o"]))

        app = self.root / "app.reo"
        app.write_text(FOREIGN_REO)
        result = self.command([COMPILER, "build", app, "--lib-dir", self.root,
                           "--link", "reo_ffi", "-o", self.root / "app"])
        self.assert_ok(result)
        self.assert_ok(self.command([self.root / "app"]))

    def test_c_calls_reo(self):
        lib = self.root / "lib.reo"
        lib.write_text(EXPORT_REO)
        result = self.command([COMPILER, "build", lib, "--shared", "-o", self.root / "libreo_exp.so"])
        self.assert_ok(result)

        harness = self.root / "harness.c"
        harness.write_text(EXPORT_HARNESS)
        exe = self.root / "harness"
        self.assert_ok(self.command(["gcc", harness, "-L", self.root, "-lreo_exp",
                                 "-Wl,-rpath," + str(self.root), "-o", exe]))
        self.assert_ok(self.command([exe]))

    def test_invalid_abi_string_is_rejected(self):
        path = self.root / "bad.reo"
        path.write_text('extern "bogus" fn f() -> i32;\nfn main() {}\n')
        result = self.command([COMPILER, "check", path])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported extern ABI", result.stderr)

    def test_c_linkage_block_form(self):
        path = self.root / "block.reo"
        path.write_text('extern "C" { fn ffi_add(a: i32, b: i32) -> i32; }\nfn main() {}\n')
        self.assert_ok(self.command([COMPILER, "check", path]))

    def test_explicit_convention_compiles_on_c_backend(self):
        # The C backend realises sysv64/win64 through the matching C attribute.
        for abi in ("sysv64", "win64"):
            path = self.root / (abi + ".reo")
            path.write_text(f'extern "{abi}" fn f() -> i32;\nfn main() {{}}\n')
            self.assert_ok(self.command([COMPILER, "build", path, "-o", self.root / abi]))

    def test_native_convention_is_target_aware(self):
        sysv = self.root / "sysv.reo"
        sysv.write_text('extern "sysv64" fn f() -> i32;\nfn main() {}\n')
        win = self.root / "win.reo"
        win.write_text('extern "win64" fn f() -> i32;\nfn main() {}\n')
        # x86-64 SysV native accepts sysv64, rejects win64.
        self.assert_ok(self.command([COMPILER, "build", sysv, "--backend", "native",
                                     "--target", "x86_64-unknown-linux-gnu", "--emit", "obj",
                                     "-o", self.root / "sysv.o"]))
        rejected = self.command([COMPILER, "build", win, "--backend", "native",
                                 "--target", "x86_64-unknown-linux-gnu", "--emit", "obj",
                                 "-o", self.root / "win.o"])
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("calling convention", rejected.stderr)
        # aarch64 native rejects sysv64 (target default is AAPCS64).
        rejected = self.command([COMPILER, "build", sysv, "--backend", "native",
                                 "--target", "aarch64-unknown-linux-gnu", "--emit", "obj",
                                 "-o", self.root / "a64.o"])
        self.assertNotEqual(rejected.returncode, 0)


if __name__ == "__main__":
    unittest.main()
