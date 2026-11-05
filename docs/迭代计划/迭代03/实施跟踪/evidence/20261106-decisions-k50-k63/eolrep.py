"""rep(path, old, new): exact single replacement that preserves the file's line endings.
old/new are written with '\n'; they are matched against the file with each '\n' allowed to be
'\r\n' or '\n' (per line), and the inserted text uses the ending of the matched region's first line."""
import re
def rep(path, old, new, count=1):
    data = open(path, 'rb').read()
    o = old.encode('utf-8'); n = new.encode('utf-8')
    pat = re.escape(o).replace(b'\\\n', b'\r?\n')
    ms = list(re.finditer(pat, data))
    assert len(ms) == count, (path, len(ms))
    out = data
    for m in reversed(ms):
        seg = m.group(0)
        eol = b'\r\n' if b'\r\n' in seg else (b'\r\n' if (data.count(b'\r\n') * 2 > data.count(b'\n')) and b'\n' not in seg else b'\n')
        out = out[:m.start()] + n.replace(b'\n', eol) + out[m.end():]
    open(path, 'wb').write(out)
