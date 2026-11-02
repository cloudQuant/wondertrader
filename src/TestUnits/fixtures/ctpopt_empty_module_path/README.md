# CTPOpt module-directory focused test

On the local macOS v8 Ninja build, run from the repository root:

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/ctpopt_empty_module_path/run.py /tmp/wt-i02-fresh-20261101-v8-build /tmp/ctpopt-path-test-new
```

The output directory must not already exist. The script recompiles the two current CTPOpt product translation units and this fixture, reuses the other v8 libraries, and builds a physical fake dylib with no SDK creator. The product TUs alone redirect `dladdr` and `dlopen` to controlled probes: each process either reports an empty module directory or a valid directory, and the `dlopen` probe delegates to the real loader. For both adapters it verifies that an empty directory plus a relative module makes zero loader calls, while an explicit absolute module or a nonempty absolute directory opens the fake dylib. Failure at the intentionally missing creator is expected. The tests do not connect to a server, use a vendor SDK, or create symlinks.

The script is tied to the local v8 Ninja configuration, Homebrew arm64 compiler/include layout, and `.dylib`; it is not a Windows/Linux test or a full clean-tree build. It writes exact commands, source hashes, and output under the requested directory. The `dladdr` probe covers the directory decision; it does not validate real module discovery.
