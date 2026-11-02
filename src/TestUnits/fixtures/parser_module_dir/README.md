# Parser module-directory focused test

This standalone macOS fixture checks ParserXTP and ParserFemas `init()` with a controlled `dladdr` result and a `dlopen` spy. It requires the project's already configured v8 Ninja build and its libraries; `WT_PARSER_V8_BUILD` may specify another compatible build. Run from the repository root:

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/parser_module_dir/build.py /tmp/parser-module-dir-test
```

For red/green comparison, optionally supply `WT_PARSER_BASELINE` containing the pre-change `ParserXTP.cpp` and `ParserFemas.cpp`; without it only current source is tested. The script uses `/tmp/wt-i03-parser-path/baseline` by default. It builds a physical fake library and copies it into each private test directory. It does not create symlinks or connect to a provider. The compiler and dynamic-loader interposition in this fixture are macOS-specific, and other platforms require independent tests.

Each parser runs three cases in separate processes:

- empty directory plus relative configured module: no call to `dlopen`;
- nonempty directory: `dlopen` receives the expected path and the fake library constructor records an actual load; the library intentionally has no SDK creator and `init()` returns false;
- empty directory plus explicitly configured absolute module: the exact existing `DLLHelper::wrap_module` result is passed to `dlopen`; this test only observes the attempted load, and does not claim an SDK was available.

Pre-change negative controls must exit 2 in the first case because the spy observed a relative-path loading attempt. Both old and current versions must pass the two controls. `commands.json` records compiler and process commands, cwd, test environment and output; `hashes.json` checks critical inputs before/after the build. Other objects and libraries come from v8 and are not a complete current-tree rebuild.
