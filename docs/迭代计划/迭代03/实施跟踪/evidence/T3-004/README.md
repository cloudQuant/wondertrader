# T3-004 ParserFemas connection-state direct retest

Isolated local arm64 fake API/callback tests. No repository source/test/CMake changes and no symlinks created. No real vendor module or server was contacted. Build uses current workspace product TU, two current test TUs and current workspace headers; all other target dependencies/configuration are reused from the existing v8 build and are not a full fresh dependency build. Physical Boost include root `/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include` contains no symlinks under `find -type l`; system shared-library paths are still used as configured by v8.

Build:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-t3-004-parser/build.py
```

Four compile/link steps exited 0; exact compiler/link argv and cwd are in `commands.json`, and logs in `build-0.log` ... `build-3.log`.

Run (30-second process bound):

```
(cd /tmp/wt-i03-t3-004-parser/run && LLVM_PROFILE_FILE=/tmp/wt-i03-t3-004-parser/run/profile.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-t3-004-parser/tests --gtest_filter='femas_parser.is_not_connected_without_api:femas_parser.connect_without_api_reports_command_not_sent:femas_parser.disconnect_without_api_reports_command_not_sent:R105FemasTest.is_connected_tracks_front_connection_callbacks:R105FemasTest.release_clears_connection_state') > /tmp/wt-i03-t3-004-parser/test.log 2>&1
```

Exit 0; five selected tests PASS, zero selected skips. The three no-API negative cases check initial connection state and command-not-sent returns. The two fake-API cases manually invoke front callbacks and check connected state and disconnect/release clearing. This does not prove actual provider connection, concurrent lifecycle safety, callback quiescence/drain after release, or another platform. No sanitizer or whole-suite regression was performed; other cases were not run.

`source-before.sha256` and `source-after.sha256` match exactly (`cmp` exit 0); these cover only the following five files, not all transitive headers:

```
4ba3fe072b2d871fb7a872f775f1bfa1004021e8212a5b318c48fa95b57c1954  src/ParserFemas/ParserFemas.cpp
c64691ef65dfb9d02a6f6d3d539bc4f2511344f7572d436fa72c09b162f8b6df  src/ParserFemas/ParserFemas.h
bcd6a92204957c09caefd491b55bad1cc7b4f8c12612aaaf5247baba939146a5  src/TestUnits/coverage/t_ParserFemas/femas_parser_callbacks_test.cpp
5a46cc82560bc68eec9e86d6163d744267891bf8eb9bf1a2c004df54762e3123  src/TestUnits/coverage/t_ParserFemas/r105_parserfemas_api_test.cpp
3409f358c8f746d593fbd6db61c0a3799fc1323f2d254b4ee320db182a8b5f2f  src/TestUnits/coverage/t_ParserFemas/r105_femas_api_stub.h
51bbdf73ab3af62f77ff0af51ff480dbf15adbb193c0bb30f4f12003f31a3d6c  product.o
7f0aa790d960a7e5f40434e7c9916764035cfd25d6ff8990a285f22931cc4a20  callbacks.o
4c907ca80cac17cd397fbbabecc6d39f547e601fbbc52383e273d09145eef9c1  r105.o
7828ce74341791a2e8a64b9543ce9b1ff788c21e96137e1110eeadeeb672eb6f  tests
```
