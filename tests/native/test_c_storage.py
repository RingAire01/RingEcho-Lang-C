"""End-to-end precise C representation and value/alias semantics."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(os.environ.get("REO_TEST_COMPILER", ROOT / "target/Debug/rev")).resolve()
LEGACY = ("__reo_arr_t", "__reo_arrf_t", "__reo_arr128_t", "__reo_array_repeat",
          "__reo_array_dup", "__reo_fn_ptr", "__reo_vec_t", "__reo_svec_t",
          "__reo_result_array_", "(int64_t)(uintptr_t)")


class StorageTests(unittest.TestCase):
    def compile(self, source, assertions="", run=True):
        with tempfile.TemporaryDirectory(prefix="reo-storage-") as directory:
            root = Path(directory)
            path, output = root / "main.reo", root / "output.c"
            path.write_text(source)
            result = subprocess.run([str(COMPILER), "build", str(path), "--target", "c-freestanding", "-o", str(output)],
                                    cwd=root, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            c = output.read_text()
            for legacy in LEGACY:
                self.assertNotIn(legacy, c)
            output.write_text(c + assertions + "\nint main(void) { main_(); return 0; }\n")
            exe = root / "test"
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("REO_TEST_SANITIZE_GENERATED") == "1" else []
            result = subprocess.run(["gcc", "-std=gnu11", "-O2", "-Werror=int-conversion", "-Werror=incompatible-pointer-types",
                                     "-Werror=cast-function-type", "-Werror=return-type", *flags, str(output), "-o", str(exe)],
                                    cwd=root, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            if run:
                result = subprocess.run([str(exe)], cwd=root, text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
            return c

    def test_inline_arrays_copy_return_and_exact_element_sizes(self):
        self.compile('''
struct Bytes { data: [u8; 3] }
struct Floats { data: [f32; 3] }
fn make() -> Bytes { return Bytes { data: [1u8, 2u8, 3u8] }; }
fn main() {
    let a = make();
    let b = a;
    b.data[0] = 9u8;
    assert(a.data[0] == 1u8);
    assert(b.data[0] == 9u8);
    let f = Floats { data: [1.5f32; 3] };
    assert(f.data[2] == 1.5f32);
    assert(len(f.data) == 3);
    let sum = 0;
    for x in a.data { sum += x as i64; }
    assert(sum == 6);
}
''', '_Static_assert(sizeof(Bytes) == 3, "inline u8 array");\n_Static_assert(sizeof(Floats) == 12, "exact f32 storage");\n')

    def test_array_and_record_references_preserve_mutations(self):
        self.compile('''
struct Record { value: i64 }
fn read(a: &[u8; 3]) -> u8 { return (*a)[1]; }
fn write(a: &mut [u8; 3]) { (*a)[1] = 7u8; }
fn read_record(p: &Record) -> i64 { return p.value; }
fn read_pointer(p: *const u8) -> u8 { return *p; }
fn main() {
    let a = [1u8, 2u8, 3u8];
    write(&mut a);
    assert(read(&a) == 7u8);
    let r = Record { value: 44 };
    assert(read_record(&r) == 44);
    let byte = 255u8;
    let p: *const u8 = &byte;
    assert(read_pointer(p) == 255u8);
}
''')

    def test_nested_inline_arrays_are_not_pointer_tables(self):
        self.compile('''
struct Matrix { values: [[u16; 2]; 2] }
fn main() {
    let m = Matrix { values: [[1u16, 2u16], [3u16, 4u16]] };
    m.values[1][0] = 9u16;
    assert(m.values[1][0] == 9u16);
    assert(m.values[0][0] == 1u16);
}
''', '_Static_assert(sizeof(Matrix) == 8, "inline matrix");\n')

    def test_function_values_keep_their_signature(self):
        c = self.compile('''
fn twice(f: fn(f64)->f64, x: f64) -> f64 { return f(f(x)); }
fn make() -> fn(f64)->f64 { return |x: f64| x + 1.25; }
fn main() {
    let f = make();
    assert(twice(f, 1.0) == 3.5);
    assert(make()(1.0) == 2.25);
    let integer = |x| x + 1;
    assert(integer(41) == 42);
}
''')
        self.assertRegex(c, r"typedef double \(\*__reo_type_\d+\)\(double\)")

    def test_vectors_use_concrete_storage(self):
        c = self.compile('''
fn main() {
    let bytes: Vec<u8> = vec_new();
    vec_push(bytes, 255u8);
    assert(vec_get(bytes, 0) == 255u8);
    let floats: Vec<f32> = vec_new();
    vec_push(floats, 1.25f32);
    assert(vec_pop(floats) == 1.25f32);
    free(bytes);
    free(floats);
    let words = svec_new();
    svec_push(words, "typed");
    assert(svec_get(words, 0) == "typed");
    svec_free(words);
}
''')
        self.assertIn("uint8_t *data; size_t len, cap;", c)
        self.assertIn("float *data; size_t len, cap;", c)

    def test_dynamic_arrays_are_typed_slices(self):
        c = self.compile('''
fn make(n: i64) -> [u8] { return [7u8; n]; }
fn main() {
    let a = make(4);
    assert(len(a) == 4);
    a[2] = 9u8;
    assert(a[2] == 9u8);
    free(a);
}
''')
        self.assertRegex(c, r"typedef struct \{ uint8_t \*data; size_t len; \}")

    def test_source_has_no_legacy_implementations(self):
        for path in (ROOT / "src/backend").glob("*.c"):
            text = path.read_text()
            for legacy in LEGACY:
                self.assertNotIn(legacy, text, str(path))

    def test_recursive_indirections_and_unit_tuple_values(self):
        self.compile('''
struct Node { next: *mut Node, children: Vec<Node>, factory: fn()->Node, value: i64 }
fn nothing() -> unit { return (); }
fn unit_callback() -> fn()->unit { return || nothing(); }
fn main() {
    let node: Node;
    node.value = 42;
    assert(node.value == 42);
    let unit_value = nothing();
    let tuple = (7u8, 1.5f32, unit_value);
    assert(tuple.f0 == 7u8);
    assert(tuple.f1 == 1.5f32);
    let callback = unit_callback();
    callback();
}
''', '_Static_assert(sizeof(Node) == 32, "typed recursive indirections");\n_Static_assert(sizeof(__reo_unit) == 0, "zero-sized unit");\n')

    def test_named_function_address_is_a_function_pointer(self):
        self.compile('''
fn value(x: i64) -> i64 { return x + 1; }
fn main() { let f = &value; assert(f(41) == 42); }
''')

    def test_empty_arrays_and_zero_sized_values(self):
        self.compile('''
fn main() {
    let empty = [];
    let bytes: [u8;0] = [];
    assert(len(empty) == 0);
    assert(bytes.len == 0);
    let units = [(); 2];
    assert(units[0] == ());
    let values: Vec<unit> = vec_new();
    vec_push(values, ());
    assert(vec_len(values) == 1);
    assert(vec_pop(values) == ());
    free(values);
}
''')

    def test_forward_records_and_alias_callback_fields(self):
        self.compile('''
fn make() -> Later { return Later { value: 42 }; }
fn callback() -> Callback { return |x: i64| x + 1; }
type Callback = fn(i64)->i64;
struct Holder { call: Callback }
struct Later { value: i64 }
fn main() {
    let r = make();
    assert(r.value == 42);
    let h = Holder { call: callback() };
    let value = h.call(41);
    assert(value == 42);
}
''')

    def test_mutable_typed_pointer_stores_and_reference_return(self):
        self.compile('''
fn update(p: &mut i64) { *p += 3; }
fn update_byte(p: *mut u8) { *p = 99u8; }
fn borrow(p: &i64) -> &i64 { return p; }
fn main() {
    let value = 39;
    update(&mut value);
    assert(*borrow(&value) == 42);
    let byte = 1u8;
    update_byte(&mut byte);
    assert(byte == 99u8);
}
''')

    def test_generic_records_and_function_values_preserve_pointer_types(self):
        self.compile('''
struct Box<T> { value: T }
fn identity<T>(value: T) -> T { return value; }
fn main() {
    let value = 42;
    let pointer = identity(&value);
    assert(*pointer == 42);
    let b = Box { value: &value };
    assert(*b.value == 42);
    let a = Box { value: [7u8, 8u8] };
    assert(a.value[1] == 8u8);
}
''')

    def test_nested_generic_and_lambda_work_queues(self):
        self.compile('''
fn inner<T>(x: T) -> T { return x; }
fn outer<T>(x: T) -> T { return inner(x); }
fn callback<T>(x: T) -> fn(T)->T { return |value: T| inner(value); }
fn main() {
    let value = 42;
    let p = outer(&value);
    assert(*p == 42);
    let f = callback(1.5);
    assert(f(2.5) == 2.5);
}
''')

    def test_immutable_writes_and_local_borrow_escapes_are_rejected(self):
        cases = [
            'fn bad(p: &i64) { *p = 1; }',
            'fn bad(p: *const u8) { *p = 1u8; }',
            'fn bad(p: &[u8;2]) { (*p)[0] = 1u8; }',
            'fn bad() -> &i64 { let x = 1; return &x; }',
            'fn bad() -> &i64 { let x = 1; let p = &x; return p; }',
            'struct Holder { p: &i64 } fn bad() -> Holder { let x = 1; return Holder { p: &x }; }',
            'fn bad() { free(42); }',
            'struct Duplicate { value: i64 } struct Duplicate { other: u8 }',
            'type Cycle = Cycle; fn bad() { let value: Cycle = 1; }',
            'fn bad(p: ptr) { *p; }',
        ]
        with tempfile.TemporaryDirectory(prefix="reo-invalid-storage-") as directory:
            path = Path(directory) / "main.reo"
            for case in cases:
                with self.subTest(case=case):
                    path.write_text(case + " fn main() {}")
                    result = subprocess.run([str(COMPILER), "check", str(path)], cwd=directory,
                                            capture_output=True, text=True, timeout=30)
                    self.assertNotEqual(result.returncode, 0, result.stderr)
                    self.assertIn("error:", result.stderr)
                    self.assertNotIn("AddressSanitizer", result.stderr)
                    self.assertNotIn("LeakSanitizer", result.stderr)
                    self.assertNotIn("runtime error:", result.stderr)


if __name__ == "__main__":
    unittest.main()
