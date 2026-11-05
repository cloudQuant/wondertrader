# Iteration 03 configuration embedded-NUL guard

From the repository root on the fixture's macOS environment:

```sh
cmake -S src/TestUnits/fixtures/i03_config_embedded_nul_20261103 \
  -B /tmp/wt-i03-config-nul-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-config-nul-20261103 -j2
LLVM_PROFILE_FILE=/tmp/wt-i03-config-nul-20261103/profile.profraw \
  /tmp/wt-i03-config-nul-20261103/i03_config_nul \
  --gtest_filter='I03ConfigNul.*:ws05_cfgloader.Parse*:ws05_cfgloader.YamlSyntaxErrorThrows:ws05_cfgloader.ContentUtf8Chinese:ws05_cfgloader.ContentGbkChineseConverted:ws05_cfgloader.RejectJson*:ws05_cfgloader.RejectYaml*' \
  --gtest_output=xml:/tmp/wt-i03-config-nul-20261103/results.xml \
  > /tmp/wt-i03-config-nul-20261103/results.log 2>&1
```

This target compiles the current WTSCfgLoader.cpp and uses the existing
WT_BUILD WTSUtils static library for its supporting implementation. It uses
the Homebrew and iconv paths listed in CMakeLists.txt; those static dependencies
are not rebuilt by this fixture. Logs contain the expected NUL diagnostics.
`baseline/` contains the source snapshots and hashes taken before this change;
`evidence/` contains results and hashes for the tested source.

The loader rejects raw NUL in input and decoded NUL in any JSON/YAML key or
string before constructing the returned Variant. The existing YAML syntax-error
exception behavior remains in force. Ordinary Unicode, escaped newline, and
null-node omission are covered. Existing source uses Variant's C-string APIs,
which cannot retain a decoded embedded NUL suffix; a static caller search found
no demonstrated binary-string dependency. External plugin usage is unverified.
These are local configuration tests, not a full regression or provider test.
