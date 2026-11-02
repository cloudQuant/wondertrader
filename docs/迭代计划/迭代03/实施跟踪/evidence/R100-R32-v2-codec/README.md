# Minute-bar V2 key codec: isolated checks

From the repository root, run:

```sh
bash src/TestUnits/fixtures/lmdb_bar_key_v2/run.sh /private/tmp/wt-bar-key-v2-check
```

The output directory must not exist. The script compiles the standalone tests
with the system compiler and with the specified LLVM 22 ASan/UBSan compiler,
enables leak detection for this standalone binary, runs only these tests, and
records compiler versions, commands (when the script's output is captured),
source hashes before/after, output logs and the symlink scan. It does not create
symbolic links, open LMDB, use market data, or call a production writer.

The proposed key consists of a canonical NUL-terminated and zero-padded
16-byte exchange, a canonical 32-byte code, and an unsigned 64-bit time in
big-endian byte order. Identities are nonempty, contain no embedded NUL, and
must fit including their NUL terminator. For the same identity, bytewise order
matches unsigned numeric time order, including `0xffffffff` and times beyond
it. Invalid input and noncanonical padding are rejected without changing the
output.

V1 candidate conversion requires an explicit **source host byte order**, an
explicit recognized 64-bit value ABI (current aligned 88-byte or historical
packed 84-byte layout), and an independently verified full timestamp. The
source host is required because V1 unconditionally byte-swapped its key field
but saved value structs in native order. The converter checks fixed sizes,
canonical V1 identity against the caller's expected exchange and code, the
value's time against that independent evidence, and the V1 key's 32-bit tail
against the low 32 bits of the value. The provenance of the independent
evidence is a **caller responsibility**: supplying a value copied from the
same record would not independently validate it. Unknown or old 32-bit value
layouts, missing evidence, corrupt padding and mismatches are rejected.

The V1 storage may already have overwritten an earlier bar under a colliding
32-bit key. Successfully converting a surviving record cannot restore or prove
the absence of such lost data. V2 keys must be placed in an explicitly
versioned, separate ordered key space; they must not be mixed with legacy
52-byte keys. The codec is not connected to any writer or reader and performs
no database migration, locking, upgrade coordination or recovery. Real LMDB
round trips, legacy databases, cross-process safety, deployment, and the overall
regression remain unverified.
