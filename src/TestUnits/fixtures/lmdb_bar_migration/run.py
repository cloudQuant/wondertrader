import json
import os
import pathlib
import subprocess
import sys

out = pathlib.Path(sys.argv[1])
variant = sys.argv[2]
modes = ("success", "reopen", "packed", "big-packed", "same", "alias", "unsafe", "limit", "missing",
         "identity", "time", "exception", "conflict", "missing-stage", "extra-stage", "crash",
         "source-change", "source-after-batch", "interrupted-batch", "badkey", "badvalue", "map-full", "map-full-partial",
         "concurrent-prepare", "unprepared", "prepare-twice", "concurrent-same", "concurrent-different", "reentrant", "reentrant-thread", "destination-change", "stat-under", "stat-over", "evidence-revoked-between-batches")
if variant == "tsan-raw":
    modes = ("concurrent-same", "concurrent-different")
elif variant == "tsan-annotated":
    modes = ("concurrent-prepare", "concurrent-same", "concurrent-different", "reentrant", "reentrant-thread", "destination-change", "stat-under", "stat-over")
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1", TSAN_OPTIONS="halt_on_error=1:exitcode=66")
results = []
for mode in modes:
    argv = [str(out / f"{variant}-test"), mode, str(out / f"{variant}-{mode}")]
    try:
        process = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=40)
        result, output = process.returncode, process.stdout + process.stderr
    except subprocess.TimeoutExpired as exc:
        result, output = "TIMEOUT", str(exc)
    (out / f"{variant}-{mode}.log").write_text(output)
    race = "WARNING: ThreadSanitizer: data race" in output
    results.append({"argv": argv, "exit": result, "tsan_race_reported": race})
    print(variant, mode, result, flush=True)
(out / f"{variant}-results.json").write_text(json.dumps(results, indent=2) + "\n")
if variant == "tsan-raw":
    # An unannotated report is preserved, not silently called a pass. The
    # independent semaphore controls establish tool behavior on this host.
    failed = any(row["exit"] != 0 and not row["tsan_race_reported"] for row in results)
else:
    failed = any(row["exit"] != 0 for row in results)
if failed:
    sys.exit(1)
