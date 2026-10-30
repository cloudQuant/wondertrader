BUG-P10-001 isolated focused build/verification

Commands were extracted using:
  /opt/homebrew/bin/ninja -C build-cov -t commands CoverageTests

Only two changed translation units were rebuilt, with their outputs and dependency files redirected to this directory:
  WtDataStorage/WtDataWriter.cpp -> WtDataWriter.o
  p10_storage_writer/p10_proc_block_data_test.cpp -> p10_proc_block_data_test.o

The temporary test binary was linked from those two fresh objects plus the other CoverageTests object files and libraries/dylibs already present in build-cov. Thus this is a focused local verification, not a clean/full rebuild; reused dependency objects and libraries may predate the current working-tree snapshot.

Build results: source exit 0, test exit 0, link exit 0.
Full P10ProcBlockTest filter: 14 passed, 1 failed. The unrelated failure is IsSessionProceededOnEmptyTable, whose test manually edits _proc_date on a default, uninitialized writer; the current writer guards this operation on lifecycle Ready.
proc_block_data focused filter (excludes only IsSessionProceededOnEmptyTable): 14/14 passed.
No sanitizer run.
