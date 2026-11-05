#!/usr/bin/env python3
"""Re-apply working-tree edits onto the HEAD version while keeping HEAD's per-line endings.

Unchanged lines keep their original bytes (CRLF or LF). Inserted/replaced lines take the
line ending of the nearest preceding HEAD line (or the file's majority ending).
"""
import subprocess, sys, difflib, collections

def main(paths):
    for p in paths:
        head = subprocess.run(['git', 'show', 'HEAD:' + p], capture_output=True, check=True).stdout
        cur = open(p, 'rb').read()
        h_lines = head.splitlines(keepends=True)
        c_lines = cur.splitlines(keepends=True)
        strip = lambda l: l.rstrip(b'\r\n')
        h_norm = [strip(l) for l in h_lines]
        c_norm = [strip(l) for l in c_lines]
        def ending(l):
            return b'\r\n' if l.endswith(b'\r\n') else (b'\n' if l.endswith(b'\n') else b'')
        counts = collections.Counter(ending(l) for l in h_lines if ending(l))
        major = counts.most_common(1)[0][0] if counts else b'\n'
        out = []
        sm = difflib.SequenceMatcher(None, h_norm, c_norm, autojunk=False)
        last_end = major
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal':
                for k in range(i1, i2):
                    out.append(h_lines[k])
                    if ending(h_lines[k]):
                        last_end = ending(h_lines[k])
            else:
                ref = ending(h_lines[i1]) if i1 < len(h_lines) and ending(h_lines[i1]) else last_end
                for k in range(j1, j2):
                    e = ending(c_lines[k])
                    out.append(c_norm[k] + (ref if e else b''))
        data = b''.join(out)
        # last line ending: mirror the current file (no newline at EOF stays so)
        open(p, 'wb').write(data)
        print(p, 'head_crlf', sum(1 for l in h_lines if l.endswith(b'\r\n')), 'new_crlf', data.count(b'\r\n'))

main(sys.argv[1:])
