from pathlib import Path
import hashlib
import sys

source = Path("src/WtBtPorter/WtBtRunner.cpp").read_bytes()
needle = b"\t\t\t_worker.reset(new StdThread("
insertion = b"\t\t\tthrow std::bad_alloc();\r\n"
assert source.count(needle) == 1
injected = source.replace(needle, insertion + needle)
assert injected.replace(insertion + needle, needle) == source
output = Path(sys.argv[1])
with output.open("xb") as stream:
    stream.write(injected)
assert output.read_bytes() == injected
print("original_sha256", hashlib.sha256(source).hexdigest())
print("injected_sha256", hashlib.sha256(injected).hexdigest())
print("verified exactly one test-only throw before worker construction")
