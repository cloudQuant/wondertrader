import difflib, sys
before_root, files = sys.argv[1], sys.argv[2:]
for f in files:
    b = open(f"{before_root}/{f}", 'rb').read()
    a = open(f, 'rb').read()
    def split(data):
        lines = data.split(b'\n')
        out = []
        for i, l in enumerate(lines):
            last = i == len(lines) - 1
            if last and l == b'':
                break
            eol = b'' if last else (b'\r\n' if l.endswith(b'\r') else b'\n')
            out.append((l[:-1] if l.endswith(b'\r') else l, eol))
        return out
    bl, al = split(b), split(a)
    crlf = sum(1 for _, e in bl if e == b'\r\n'); lf = sum(1 for _, e in bl if e == b'\n')
    default = b'\r\n' if crlf >= lf else b'\n'
    sm = difflib.SequenceMatcher(None, [x for x, _ in bl], [x for x, _ in al], autojunk=False)
    out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1):
                text, _ = al[j1 + k]
                out.append(text + (bl[i1 + k][1] if al[j1 + k][1] != b'' else b''))
        else:
            for k in range(j1, j2):
                text, e = al[k]
                out.append(text + (default if e != b'' else b''))
    open(f, 'wb').write(b''.join(out))
    nb = open(f, 'rb').read()
    print(f, 'crlf_lines', nb.count(b'\r\n'), 'lf_only', nb.count(b'\n') - nb.count(b'\r\n'))
