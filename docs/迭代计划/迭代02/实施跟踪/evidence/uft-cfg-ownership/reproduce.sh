#!/usr/bin/env bash
set -euo pipefail
repo_root="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
cd "$repo_root"
/opt/homebrew/bin/cmake --build build-cov --target CoverageTests_uftcore TraderMocker -j 4 --verbose
./build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_uftcore '--gtest_filter=UftTraderAdapterLifecycle.*'
