# BUG-L00-010 CTPOpt module-path sub-slice

Current ParserCTPOpt and TraderCTPOpt source checks an empty `getBinDir()` before loading: relative module names fail with an error log and no `dlopen`; explicitly absolute module filenames remain eligible for direct loading; nonempty-directory behavior is unchanged. This does not audit other adapters or prove every nonempty directory is absolute. The parser also checks before creating its flow directory.

No real API or server is contacted. The fixture redirects only the two product translation units' `dladdr` and `dlopen` calls to a controlled probe; the loader probe records calls and then delegates to the real OS loader. A separately compiled physical fake dylib deliberately lacks the API creator; `init` is expected to return false after successful `dlopen`. It verifies:

| Source | Empty directory + relative module | Empty directory + explicit absolute module | Valid absolute directory + relative module |
| --- | --- | --- | --- |
| Current ParserCTPOpt | PASS; zero loader calls | PASS; actual fake dylib opened | PASS; actual fake dylib opened |
| Current TraderCTPOpt | PASS; zero loader calls | PASS; actual fake dylib opened | PASS; actual fake dylib opened |
| Original dirty-tree copy, each adapter | FAIL as intended; spy saw a relative `dlopen` | PASS; actual fake dylib opened | PASS; actual fake dylib opened |

Current source run via repository fixture:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/ctpopt_empty_module_path/run.py /tmp/wt-i02-fresh-20261101-v8-build /tmp/wt-i03-ctpopt-path-final
```

Final source logs, command argument vectors and SHA-256 are in `/tmp/wt-i03-ctpopt-path-final/` (`commands.json`, `hashes.json`, `*-current-{empty,absolute,valid}.log`). Earlier baseline and current commands, logs and original product copies are in this directory (`build.py`, `commands.json`, `baseline/`, `*-baseline-*.log`). `symlinks-final.log` is empty. `git -c core.whitespace=cr-at-eol diff --check` passed on product files.

This is local macOS arm64 evidence, using current targeted translation units and reused v8 libraries; not a clean full-tree build. Other product objects, vendor SDK, actual server, real `dladdr`, concurrency, Windows and Linux are not verified. The spy sees relative `dlopen` in baseline; it does not require the relative library to exist. The absolute/nonempty positive controls perform a real `dlopen` of a physical fake, but do not establish successful vendor API initialization.
