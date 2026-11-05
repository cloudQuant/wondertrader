import json
import os
import pathlib
import subprocess
import sys

out = pathlib.Path(sys.argv[1])
env = dict(os.environ, TSAN_OPTIONS="halt_on_error=1:exitcode=66")
rows = []
for mode in ("raw", "annotated", "unprotected"):
    argv = [str(out / "tsan-sem-control"), mode]
    try:
        process = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=20)
        code, text = process.returncode, process.stdout + process.stderr
    except subprocess.TimeoutExpired as exc:
        code, text = "TIMEOUT", str(exc)
    (out / f"tsan-sem-{mode}.log").write_text(text)
    race = "WARNING: ThreadSanitizer: data race" in text
    rows.append({"argv": argv, "exit": code, "tsan_race_reported": race})
    print("tsan-sem", mode, code, "race", race, flush=True)
(out / "tsan-sem-results.json").write_text(json.dumps(rows, indent=2) + "\n")
if rows[1]["exit"] != 0 or any(rows[i]["exit"] == 0 or not rows[i]["tsan_race_reported"] for i in (0, 2)):
    sys.exit(1)
