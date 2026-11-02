# L00-010 ParserXTP / ParserFemas local path guard

2026-11-01 macOS arm64. Current source adds a guard before path creation and DLL loading when getBinDir is empty and the wrapped module path is relative; `std::filesystem::path::is_absolute` preserves the existing wrapped absolute-module load attempt. Nonempty getBinDir still concatenates as before. Logging reports the unavailable module directory; both init methods return false. No other parser implementations were edited.

From the repository root:

    /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/parser_module_dir/build.py /tmp/wt-i03-parser-path/final2
    /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-parser-path/production_compile.py

`commands.json` records exact arguments, output, cwd, environment and exit status. For each parser, pre-change empty directory + relative path exits 2 because the dlopen spy observed an attempt. Current source exits 0 only when no loading attempt occurred. Pre-change and current valid-directory cases each actually load an isolated physical fake dylib and verify its constructor marker; the creator is intentionally missing so initialization returns false. Pre-change and current empty-directory + explicit-absolute cases confirm that the existing wrapped absolute path reaches dlopen unchanged; that target is intentionally absent, so this is an attempted-load control rather than a successful provider load. Thus current 6/6 and baseline positive controls 4/4 pass; 2/2 baseline negative controls detect the defect. Hashes before/after are in `hashes.json` and baseline hashes in `baseline.sha256`; `xtp-own.diff` and `femas-own.diff` isolate this work from preexisting dirty changes. `symlinks.log` is empty.

Other objects/libraries and build configuration are reused from v8. No actual provider was contacted; no Linux or Windows compilation/runtime, sanitizer, full current-tree build, other parser call sites, or overall regression was performed. Absolute path interpretation is std::filesystem platform dependent. The fixture uses compile-time dladdr/dlopen symbol interception and is not a production build; `production-commands.json` separately records compile results without those substitutions.
