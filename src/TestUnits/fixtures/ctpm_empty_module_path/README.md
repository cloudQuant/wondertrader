# CTPMini module-directory focused test

On macOS arm64, first configure and build `CoverageTests_t_ParserCTPMini` and
`CoverageTests_t_TraderCTPMini` in an isolated Ninja build directory using a
physical Boost include directory. Then run from the repository root:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/ctpm_empty_module_path/run.py /path/to/ninja-build /tmp/ctpm-path-test-new
```

Use a new output directory for a fresh run. The script can resume a partial run
in the same directory by reusing successful command records; do not reuse it
after source or build inputs change. It separately compiles the two product TUs,
fixture, and corresponding fake-API test TUs, links against the supplied build's
dependencies, and executes only focused tests. `dladdr` and `dlopen` are
redirected only in the product TUs. The valid-directory positive calls the real
loader on a physical fake dylib; the absolute-path case verifies the attempted
wrapped path. The loader probe then exits because the existing ParserCTPMini
missing-creator branch can call a null pointer; that issue is outside this path
change. Empty-relative mode requires no load and invokes connect afterward.
The fixture does not connect to a real SDK, create symlinks, or establish
full-tree or cross-platform behavior.
