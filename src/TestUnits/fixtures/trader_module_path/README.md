# Trader module directory focused spy

Run on macOS with the user's Anaconda Python and an existing compatible Ninja build:

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/trader_module_path/build_and_run.py /tmp/wt-i02-fresh-20261101-v8-build /tmp/trader-path-test
```

The program recompiles each trader's current translation unit with dedicated `dladdr`, `dlopen`, `dlsym` and `dlclose` spies; it recompiles the test source and reuses the selected build's other libraries and link configuration. It tests an empty module directory, a nonempty directory, and an already absolute wrapped module name. Femas additionally tests its pre-existing default bare-name branch. For XTP the fake creator returns null and records invocation; the no-creator branch calls `connect` to check that the new failure path does not start a vendor API. No real SDK is called, and no symlinks are created.

A fourth optional argument points to a directory containing pre-change `TraderXTP.cpp` and `TraderFemas.cpp`. In that mode, the empty-relative cases must fail while the other cases must pass. Exact commands, process exits and output are recorded in the output directory.

The spy exercises a slash-free module image name to obtain an empty `getBinDir`; it does not establish operating system loader, SDK, or cross-platform behavior. XTP's historical `DLLHelper::wrap_module` transformation is intentionally preserved, including the way its `lib` prefix transforms an absolute name on Unix. An already nonempty directory is not asserted to be absolute. Module unloading and the other trader/parser adapters remain separate work.
