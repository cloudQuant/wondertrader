# L04-004 LMDB identity boundaries

On the recorded macOS environment, run:

```sh
src/TestUnits/fixtures/l04_lmdb_key_bounds/run.sh /tmp/wt-i03-new084-build
```

The supplied build must be a configured Ninja build of this project with the
`WtDataStorageAD` target and a built `build_x64/Debug/libs/libWTSUtils.a`.
The script compiles the four current AD translation units using that build's
flags, links the product-entry probe against its existing WTSUtils library,
and separately runs key-layout and boundary checks with LLVM ASan/UBSan. It
leaves its uniquely named output directory under `/tmp` for inspection.
Compiler and library paths reflect this host and may need adjustment elsewhere.
The mixed-source link is not a full current-tree build. These tests do not
establish asynchronous persistence, restart recovery, or cross-platform ABI.
