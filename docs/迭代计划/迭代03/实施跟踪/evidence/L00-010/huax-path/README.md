# HuaX module-directory focused fixture

On macOS arm64 with the specified local Homebrew versions installed, run from the repository root:

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/huax_module_path/build_and_run.py /tmp/wt-huax-focused-final
```

The build compiles current ParserHuaX and TraderHuaX translation units independently and a baseline copy produced by removing only the newly added empty-directory guard. It intercepts `dladdr`, `dlopen`, `dlsym`, and `dlclose` in those units; each process tests the default and configured relative module names with an empty directory, a nonempty directory, or a wrapped absolute module path. The spy checks the actual path and creator/API calls; it does not load a real module or contact any vendor. Positive absolute input passes through the existing `wrap_module` transformation, so the resulting recorded path is `/libisolated/absolute.dylib`, not the unwrapped input.

In the current test the empty-directory cases return failure without loader, creator, or API calls, and a direct `connect()` and `release()` after failure are safe. The guard-only baseline intentionally attempts a relative load. Other modes validate the current and baseline existing successful behavior with a fake API. Successful trader `connect()` starts a preexisting non-terminating worker; that call runs in a child process which uses `_Exit` after the API assertions. The parent checks init/release/destruction without starting that worker. These tests do not establish a safe worker shutdown or unload contract.

The trader TU has MSVC-style `strcpy_s` overloads outside this path; the fixture uses `posix_strcpy_compat.hpp` to compile it on macOS. This is a test-only shim and does not establish a production POSIX build. The focused executable uses `-undefined dynamic_lookup` for uncalled application dependencies; it is not a full application link. It uses physical Boost static archives and creates no symlinks. No Windows/Linux, sanitizer, real provider, full dependency build, or overall regression is claimed.

`commands.json`, individual `*.log` files, `hashes.json`, and `*-baseline.diff` in the output directory record commands, process exits, source stability and the exact baseline transformation. A nonzero expected exit for each baseline empty-directory case demonstrates the earlier path attempted a relative load; it is not a reproduction of all original dirty-tree behavior.
