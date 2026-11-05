#!/bin/bash
# Overlay the dt-writer package files from the live repository onto the after snapshot.
set -euo pipefail
repo=/Users/yunjinqi/Documents/new_projects/wondertrader
A=/tmp/wt-i03-s5-dt-writer-after
files=(src/WtDtHelper/WtDtHelper.cpp src/WtDtHelper/WtDtHelper.h src/Includes/IDataWriter.h
 src/WtDataStorage/WtDataWriter.cpp src/WtDataStorage/WtDataWriter.h
 src/WtDtCore/DataManager.cpp src/WtDtCore/DataManager.h
 src/TestUnits/fixtures/r18_writer_fixture.cpp
 src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_helper_v2_filter_test.cpp
 src/TestUnits/coverage/q05_dtsrv/q05_resample_leak_paths_test.cpp)
for f in "${files[@]}"; do
  if ! cmp -s "$repo/$f" "$A/$f"; then cp -p "$repo/$f" "$A/$f"; echo "overlaid $f"; fi
done
(cd "$A" && shasum -a 256 "${files[@]}") > "$A/overlay-sha256.txt"
