"""Shift-JIS string literals for the PC build.

The decomp sources are UTF-8. The GameCube build runs MWCC through sjiswrap,
so narrow string and character literals with Japanese text end up Shift-JIS
in the binary, matching the game data (JDrama object names in scene.bin and
stageArc.bin, BMG keys, ...). clang only knows UTF-8 (-fexec-charset accepts
nothing else), so without this every Japanese literal would be UTF-8 and every
lookup by Japanese name (TNameRefGen::search etc.) would fail.

mirror() writes a copy of each source/header that has such literals to
port/build/sjis/<same relative path>, with every non-ASCII character inside a
narrow literal replaced by its cp932 bytes as 3-digit octal escapes (3 digits
so a following digit is never absorbed). Comments, wide/UTF literals and
everything else are left alone. Quoted #includes that resolve next to the
original file are rewritten to the original's absolute path, so the copy
includes exactly what the original did. The build compiles the copy instead of
the original and puts the mirror include roots in front of the real ones.
Files are only rewritten when their content changes, so ninja stays
incremental; mirrors whose original no longer needs one are deleted.
"""
import os
import re
from pathlib import Path

EXTS = (".c", ".cpp", ".h", ".hpp", ".inc", ".pch")
INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)"([^"]+)"', re.M)


def _encode_char(ch):
    try:
        b = ch.encode("cp932")
    except UnicodeEncodeError:
        b = ch.encode("shift_jis", errors="replace")
    return "".join("\\%03o" % x for x in b)


def convert_text(s):
    """Return the converted text, or None if nothing needed converting."""
    out = []
    i = 0
    n = len(s)
    last = 0
    changed = False
    while i < n:
        c = s[i]
        if c == "/" and s.startswith("//", i):
            j = s.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and s.startswith("/*", i):
            j = s.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"' or c == "'":
            # prefix: L"", u"", U"", u8"" are not narrow literals
            k = i - 1
            prefix = ""
            while k >= 0 and (s[k].isalnum() or s[k] == "_"):
                prefix = s[k] + prefix
                k -= 1
            j = i + 1
            while j < n and s[j] != c and s[j] != "\n":
                if s[j] == "\\":
                    j += 1
                j += 1
            lit = s[i:j + 1]
            if prefix in ("", ) and any(ord(ch) > 127 for ch in lit):
                out.append(s[last:i])
                out.append("".join(_encode_char(ch) if ord(ch) > 127 else ch for ch in lit))
                last = j + 1
                changed = True
            i = j + 1
            continue
        i += 1
    if not changed:
        return None
    out.append(s[last:])
    return "".join(out)


def _fix_includes(text, orig):
    d = orig.parent

    def rep(m):
        target = d / m.group(2)
        if target.exists():
            return m.group(1) + '"' + target.resolve().as_posix() + '"'
        return m.group(0)

    return INCLUDE_RE.sub(rep, text)


def mirror(root, dirs, out):
    """Mirror every file under root/<dirs> that needs conversion into out.
    Returns the set of mirrored relative paths (posix)."""
    root = Path(root)
    out = Path(out)
    done = set()
    for d in dirs:
        base = root / d
        if not base.exists():
            continue
        for dp, _, files in os.walk(base):
            for f in files:
                if not f.endswith(EXTS):
                    continue
                p = Path(dp) / f
                raw = p.read_bytes()
                if max(raw, default=0) < 0x80:
                    continue
                try:
                    s = raw.decode("utf-8")
                except UnicodeDecodeError:
                    continue
                t = convert_text(s)
                if t is None:
                    continue
                t = _fix_includes(t, p)
                rel = p.relative_to(root).as_posix()
                dst = out / rel
                data = t.encode("utf-8")
                if not dst.exists() or dst.read_bytes() != data:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    dst.write_bytes(data)
                done.add(rel)
    # drop stale mirrors
    if out.exists():
        for dp, _, files in os.walk(out):
            for f in files:
                p = Path(dp) / f
                rel = p.relative_to(out).as_posix()
                if rel not in done:
                    p.unlink()
    return done
