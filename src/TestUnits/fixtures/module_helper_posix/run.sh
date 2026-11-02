#!/bin/sh
set -eu
src_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
fixture="$src_root/TestUnits/fixtures/module_helper_posix"
build_dir=${1:?pass an isolated build directory}
mkdir -p "$build_dir"
build_dir=$(CDPATH= cd -- "$build_dir" && pwd -P)
include_root=${WT_MODULE_HELPER_TEST_INCLUDE_ROOT:-$src_root}
compiler=${CXX:-c++}
"$compiler" -std=c++17 -pthread -I "$include_root" "$fixture/fake_dladdr_test.cpp" -o "$build_dir/fake_dladdr_test"
case $(uname -s) in
    Darwin)
        module="$build_dir/plugin.dylib"
        "$compiler" -std=c++17 -pthread -I "$include_root" "$fixture/real_dladdr_test.cpp" -o "$build_dir/real_dladdr_test"
        "$compiler" -std=c++17 -dynamiclib -I "$include_root" "$fixture/plugin.cpp" -o "$module"
        ;;
    *)
        module="$build_dir/plugin.so"
        "$compiler" -std=c++17 -pthread -I "$include_root" "$fixture/real_dladdr_test.cpp" -ldl -o "$build_dir/real_dladdr_test"
        "$compiler" -std=c++17 -fPIC -shared -I "$include_root" "$fixture/plugin.cpp" -o "$module"
        ;;
esac
for mode in failure null empty valid no_separator concurrent; do
    perl -e 'alarm 15; exec @ARGV' -- "$build_dir/fake_dladdr_test" "$mode"
done
perl -e 'alarm 15; exec @ARGV' -- "$build_dir/real_dladdr_test" "$module"
