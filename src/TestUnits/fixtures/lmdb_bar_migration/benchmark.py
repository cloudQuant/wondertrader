"""Single-run synthetic timing; the output directory must come from run.sh."""
import json
import pathlib
import subprocess
import sys

out = pathlib.Path(sys.argv[1]).resolve()
rows = []
for count in (10000, 50000, 100000, 250000):
    argv = [str(out / "normal-test"), f"benchmark-{count}", str(out / f"benchmark-{count}")]
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=120)
        code, text = result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired as exc:
        code, text = "TIMEOUT", str(exc)
    (out / f"benchmark-{count}.log").write_text(text)
    rows.append({"argv": argv, "exit": code, "output": text.strip()})
    print(count, code, text.strip(), flush=True)
(out / "benchmark-results.json").write_text(json.dumps(rows, indent=2) + "\n")
if any(r["exit"] != 0 for r in rows):
    sys.exit(1)
