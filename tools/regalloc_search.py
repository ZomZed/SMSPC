"""C++-safe brute-force search over semantics-preserving source shapes that only move MWCC's
register / stack-slot allocation (decomp-permuter cannot parse our C++).

python tools/regalloc_search.py --unit Enemy/Kumokun --func "TNerveKumokunWait::execute(TSpineBase<TLiveActor>*) const"
python tools/regalloc_search.py --list targets.txt        # lines: <unit><TAB><full signature>
Options: --max-compiles N (default 80), --seed N, --no-swap, --dry-run (print knobs and the first
candidates, compile nothing), --keep-equal (never used: only strictly better results are kept).

Knobs (text level, all semantics preserving):
  * move a simple scalar/pointer local declaration without initializer (`f32 t;`, `TActor* p;`,
    `JGeometry::TVec3<f32> v;`) to any top-level position between the body start and its first use;
  * split `T x = expr;` into `T x; x = expr;` (and join the reverse shape) for non-const scalars /
    pointers, the split declaration then being movable as above;
  * swap the operands of `a + b` / `a * b` when both are scalar locals / params / literals and
    the operation is not part of a longer chain of the same precedence.
#if/#ifdef chains of pure defined() tests (SMS_PORT, VERSION_*) are collapsed to their GC branch (candidates that edit a GC
branch or move a declaration a port branch uses across it are rejected, the port text is restored verbatim);
bodies with other preprocessor lines, goto, or non-ASCII bytes in non-UTF-8 files are left alone. The function body is found by
brace matching (strings / comments skipped); DEFINE_NERVE-style macro heads are recognised. The file is
read and written as latin-1 bytes (Shift-JIS stays untouched). Per candidate only that object is rebuilt
and decomp-diff is parsed; a candidate is rejected when any other function of the unit drops. The file
is restored unless a strictly better shape was found (score = (match %, -diff lines)).
"""
import argparse, itertools, os, random, re, subprocess, sys, time, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable

SCALARS = set("""int u8 u16 u32 u64 s8 s16 s32 s64 f32 f64 float double bool BOOL char short long unsigned
size_t JGeometry::TVec3<f32> TVec3<f32> JGeometry::TVec3<float> Vec Mtx MtxPtr""".split())
NUMERIC = set("int u8 u16 u32 u64 s8 s16 s32 s64 f32 f64 float double short long unsigned size_t".split())
KEYWORDS = set("return delete goto break continue case throw else new sizeof typedef static extern register".split())

# ----------------------------------------------------------------------------- source scanning

def skip_token(s, i):
    """If s[i] starts a string / char literal / comment, return index after it, else i."""
    c = s[i]
    if c == '"' or c == "'":
        j = i + 1
        while j < len(s) and s[j] != c:
            j += 2 if s[j] == '\\' else 1
        return j + 1
    if s.startswith("//", i):
        j = s.find("\n", i)
        return len(s) if j < 0 else j
    if s.startswith("/*", i):
        j = s.find("*/", i + 2)
        return len(s) if j < 0 else j + 2
    return i


GC_DEFINES = {"VERSION_GMSE01", "VERSION_GMSP01", "GEKKO", "NDEBUG", "BUILD_VERSION", "__MWERKS__"}  # GMSE01 cflags
ALLOW_NONASCII = False   # set per file: UTF-8 sources never put ASCII bytes inside multi-byte chars


def pp_eval(kind, cond):
    """Value of a #if/#ifdef/#ifndef/#elif condition in the GC build, or None when it is not a pure
    defined()/literal expression."""
    cond = re.sub(r"//.*|/\*.*?\*/", "", cond).strip()
    if kind in ("ifdef", "ifndef"):
        if not re.match(r"^\w+$", cond):
            return None
        return (cond in GC_DEFINES) == (kind == "ifdef")
    e = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
               lambda m: " 1 " if (m.group(1) or m.group(2)) in GC_DEFINES else " 0 ", cond)
    if re.search(r"[A-Za-z_]", e):
        return None
    e = e.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    try:
        return bool(eval(e, {"__builtins__": {}}))
    except Exception:
        return None


def collapse_port(text):
    """GC view of a source: every #if/#ifdef/#ifndef chain whose conditions are pure defined() tests becomes
    `/*@PPn<@*/<branch the GC build compiles>/*@PPn>@*/`. Returns (gc_text, regions);
    regions[n] = (orig, gc_branch, ids used in the other branches). Other preprocessor lines stay
    (the Model still refuses bodies that contain them)."""
    lines = text.splitlines(True)
    out, regions, i = [], [], 0
    while i < len(lines):
        m = re.match(r"\s*#\s*(if|ifdef|ifndef)\b(.*)", lines[i])
        if not m or pp_eval(m.group(1), m.group(2)) is None:
            out.append(lines[i]); i += 1; continue
        depth, j, heads = 0, i, []
        while j < len(lines):
            d = re.match(r"\s*#\s*(\w+)(.*)", lines[j])
            if d:
                if d.group(1) in ("if", "ifdef", "ifndef"):
                    depth += 1
                    if depth == 1:
                        heads.append((j, d.group(1), d.group(2)))
                elif d.group(1) == "endif":
                    depth -= 1
                    if depth == 0:
                        break
                elif d.group(1) in ("else", "elif") and depth == 1:
                    heads.append((j, d.group(1), d.group(2)))
            j += 1
        if j >= len(lines):
            out.append(lines[i]); i += 1; continue
        branches, taken, ok = [], None, True
        for k, (ln, kind, cond) in enumerate(heads):
            end = heads[k + 1][0] if k + 1 < len(heads) else j
            body = "".join(lines[ln + 1:end])
            v = True if kind == "else" else pp_eval("if" if kind == "elif" else kind, cond)
            if v is None:
                ok = False; break
            if v and taken is None:
                taken = len(branches)
            branches.append(body)
        if not ok:
            out.append(lines[i]); i += 1; continue
        gc = branches[taken] if taken is not None else ""
        other = "".join(b for k, b in enumerate(branches) if k != taken)
        n = len(regions)
        regions.append(("".join(lines[i:j + 1]), gc, set(re.findall(r"[A-Za-z_]\w*", strip_comments(other)))))
        out.append("/*@PP%d<@*/%s/*@PP%d>@*/\n" % (n, gc, n))
        i = j + 1
    return "".join(out), regions


def blank_comments(s):
    """Comments replaced by spaces of the same length (offsets stay valid; the @PP markers are comments)."""
    out, i = [], 0
    while i < len(s):
        j = skip_token(s, i)
        if j != i:
            out.append(s[i:j] if s[i] in "\"'" else re.sub(r"[^\n]", " ", s[i:j])); i = j
        else:
            out.append(s[i]); i += 1
    return "".join(out)


def expand_port(gc_text, regions, orig_gc_body=None, new_gc_body=None, local_names=None):
    """Inverse of collapse_port. Returns None (reject the candidate) when a GC branch was edited, a marker
    moved out of order, or a local the search can move (local_names) that a port branch uses crossed that region."""
    pos, out, last = [], [], 0
    for n, (orig, gc, ids) in enumerate(regions):
        a, b = "/*@PP%d<@*/" % n, "/*@PP%d>@*/\n" % n
        s = gc_text.find(a, last)
        e = gc_text.find(b, s + len(a)) if s >= 0 else -1
        if s < 0 or e < 0 or gc_text.count(a) != 1:
            return None
        cur = gc_text[s + len(a):e]
        if cur != gc:
            # a region that encloses the whole function (e.g. `#if VERSION ... #endif` around it): the edit
            # lands in the GC branch; write it back there when that branch text is unambiguous
            if not gc or orig.count(gc) != 1 or "/*@PP" in cur:
                return None
            orig = orig.replace(gc, cur)
        out.append(gc_text[last:s]); out.append(orig); last = e + len(b)
    out.append(gc_text[last:])
    if orig_gc_body is not None:
        ob, nb = blank_comments(orig_gc_body), blank_comments(new_gc_body)
        for n, (orig, gc, ids) in enumerate(regions):
            a = "/*@PP%d<@*/" % n
            mo, mn = orig_gc_body.find(a), new_gc_body.find(a)
            if mo < 0:
                continue
            for name in (ids & local_names if local_names is not None else ids):
                fo = re.search(r"\b%s\b" % re.escape(name), ob)
                fn = re.search(r"\b%s\b" % re.escape(name), nb)
                if fo and fn and (fo.start() < mo) != (fn.start() < mn):
                    return None
    return "".join(out)


def match_brace(s, i, open_c, close_c):
    """s[i] == open_c; return index of the matching close_c."""
    depth = 0
    while i < len(s):
        j = skip_token(s, i)
        if j != i:
            i = j
            continue
        c = s[i]
        if c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced %s" % open_c)


def strip_comments(s):
    out, i = [], 0
    while i < len(s):
        j = skip_token(s, i)
        if j != i:
            if s[i] in "\"'":
                out.append(s[i:j])
            else:
                out.append(" ")
            i = j
        else:
            out.append(s[i]); i += 1
    return "".join(out)


def top_commas(s):
    depth, n = 0, 0
    for c in s:
        if c in "(<[":
            depth += 1
        elif c in ")>]":
            depth -= 1
        elif c == "," and depth == 0:
            n += 1
    return n


def locate_body(text, sig):
    """Return (body_start, body_end, params_text) for the function definition of `sig`;
    body is text[body_start:body_end] (inside the braces)."""
    name = sig.split("(")[0].strip()
    want_commas = top_commas(sig[sig.find("(") + 1:sig.rfind(")")]) if "(" in sig else -1
    cands = []
    for m in re.finditer(r"(?<![\w:])" + re.escape(name) + r"\s*\(", text):
        ls = text.rfind("\n", 0, m.start()) + 1
        pre = text[ls:m.start()]
        if ";" in pre or "=" in pre or "return" in pre or "(" in pre:
            continue
        if pre.strip() == "" and ls > 0:
            # name at column 0: only a definition if the previous line is a return type (no ; at end)
            pass
        p_open = m.end() - 1
        try:
            p_close = match_brace(text, p_open, "(", ")")
        except ValueError:
            continue
        params = text[p_open + 1:p_close]
        if want_commas >= 0 and params.strip() and top_commas(params) != want_commas:
            continue
        if want_commas >= 0 and not params.strip() and want_commas != 0:
            continue
        # skip to '{' or ';' (mem-init lists allowed)
        i = p_close + 1
        while i < len(text):
            j = skip_token(text, i)
            if j != i:
                i = j; continue
            c = text[i]
            if c == "{":
                break
            if c == ";":
                i = -1; break
            if c == "(":
                i = match_brace(text, i, "(", ")") + 1; continue
            i += 1
        if i < 0 or i >= len(text):
            continue
        cands.append((i, params))
    if not cands:
        cls = name.rsplit("::", 1)[0] if "::" in name else None
        if cls:
            for m in re.finditer(r"(?m)^[A-Z_][A-Z0-9_]*\(\s*" + re.escape(cls) + r"\s*[,)][^\n;]*\)\s*\n?\s*\{", text):
                cands.append((m.end() - 1, None))
    if not cands:
        raise ValueError("definition of %s not found" % sig)
    if len(cands) > 1:
        print("  warning: %d candidate definitions, using the first" % len(cands))
    b_open, params = cands[0]
    b_close = match_brace(text, b_open, "{", "}")
    return b_open + 1, b_close, params


def brace_blocks(stmt):
    """(open, close) index pairs of the top-level {...} blocks of a compound statement."""
    out, i, depth = [], 0, 0
    while i < len(stmt):
        j = skip_token(stmt, i)
        if j != i:
            i = j; continue
        c = stmt[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "{" and depth == 0:
            e = match_brace(stmt, i, "{", "}")
            out.append((i, e)); i = e
        i += 1
    return out


def split_statements(body):
    """Split a body into top-level statements (each keeps its leading whitespace / comments)."""
    stmts, start, i, depth = [], 0, 0, 0
    while i < len(body):
        j = skip_token(body, i)
        if j != i:
            i = j; continue
        c = body[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                # compound statement ends unless followed by else / while / catch
                k = i + 1
                while k < len(body) and body[k] in " \t\r\n":
                    k += 1
                # `= { 0, 2 };` initializers and `do {} while (x);` continue to the ';'
                if body[k:k + 1] != ";" and not re.match(r"(else|while|catch)\b", body[k:k + 6]):
                    stmts.append(body[start:i + 1]); start = i + 1
        elif c == ";" and depth == 0:
            stmts.append(body[start:i + 1]); start = i + 1
        i += 1
    tail = body[start:]
    return stmts, tail


DECL_RE = re.compile(
    r"^(?P<q>(?:const\s+|volatile\s+)*)(?P<type>[A-Za-z_][\w:]*(?:<[^;{}=()]*>)?(?:::\w+)?)\s*"
    r"(?P<ptr>[*&]+)?\s*(?P<names>[A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*"
    r"(?:=\s*(?P<init>.+)|\((?P<cinit>.+)\))?\s*;$", re.S)


def analyse_stmt(stmt):
    """Return dict describing a declaration statement or None."""
    core = strip_comments(stmt).strip()
    if "\n" in core and not core.endswith(";"):
        return None
    m = DECL_RE.match(core)
    if not m:
        return None
    t = m.group("type")
    if t in KEYWORDS or t.split("::")[0] in KEYWORDS or "[" in core.split("=")[0]:
        return None
    names = [n.strip() for n in m.group("names").split(",")]
    ptr = m.group("ptr") or ""
    if "&" in ptr:
        return None
    is_ptr = "*" in ptr
    scalar = is_ptr or t in SCALARS
    const = "const" in m.group("q")
    if "volatile" in m.group("q"):
        return None
    init = m.group("init")
    cinit = m.group("cinit")
    return dict(type=m.group("q") + t + ptr, names=names, scalar=scalar, const=const,
                numeric=(not is_ptr and t in NUMERIC), init=init, cinit=cinit, ptr=is_ptr)


def uses(stmt, names):
    s = strip_comments(stmt)
    return any(re.search(r"(?<![\w.>])" + re.escape(n) + r"\b", s) for n in names)


def leading_ws(stmt):
    """Leading whitespace and comments of a statement (kept in front of it when it is re-rendered)."""
    i = 0
    while i < len(stmt):
        if stmt[i] in " \t\r\n":
            i += 1; continue
        j = skip_token(stmt, i)
        if j != i and stmt[i] not in "\"'":
            i = j; continue
        break
    return stmt[:i]

# ----------------------------------------------------------------------------- the model

class Model:
    """Fixed statements F plus knobs. See module doc."""

    def __init__(self, body, params, do_swap=True, known=None):
        self.ok = True
        self.reason = ""
        self.subs = {}         # F index -> (pieces, [sub Models]) for compound statements
        self.known_numeric = set(known or ())
        if not ALLOW_NONASCII and any(ord(c) > 127 for c in body):
            self.ok, self.reason = False, "non-ASCII bytes in body"; return
        stripped = strip_comments(body)
        if re.search(r"(?m)^\s*#", stripped) or re.search(r"\bgoto\b", stripped):
            self.ok, self.reason = False, "preprocessor / goto in body"; return
        # labels at this block's own level (switch bodies, goto targets): leave the block alone
        own = stripped
        for (o, c) in reversed(brace_blocks(stripped)):
            own = own[:o + 1] + own[c:]
        if re.search(r"(?m)^\s*(case\b|default\s*:|\w+\s*:(?!:))", own):
            self.ok, self.reason = False, "labels in body"; return
        stmts, self.tail = split_statements(body)
        if not stmts:
            self.ok, self.reason = False, "empty body"; return
        self.indent = leading_ws(stmts[0]) if stmts else "\n\t"
        infos = [analyse_stmt(s) for s in stmts]
        if params:
            for p in params.split(","):
                pm = re.match(r"\s*(?:const\s+)?([\w:]+(?:<[^>]*>)?)\s*([*&]*)\s*(\w+)\s*$", p)
                if pm and not pm.group(2) and pm.group(1) in NUMERIC:
                    self.known_numeric.add(pm.group(3))
        for inf in infos:
            if inf and inf["numeric"] and len(inf["names"]) == 1 and not inf["cinit"]:
                self.known_numeric.update(inf["names"])
        # Build fixed list F and movable decls.
        self.F = []            # list of fixed statement texts (initdecl placeholders as ("init", k))
        self.movable = []      # dicts: text, names, orig_anchor, firstuse (computed later)
        self.initdecls = []    # dicts: type, name, expr, ws, default_split, orig_anchor(of decl, when split)
        used_so_far = []       # statements seen so far (for join check)
        i = 0
        while i < len(stmts):
            st, inf = stmts[i], infos[i]
            if inf and inf["scalar"] and inf["init"] is None and inf["cinit"] is None and not inf["const"]:
                # plain movable decl; join check: next use is `x = expr;` for a single-name decl
                if len(inf["names"]) == 1:
                    n = inf["names"][0]
                    j = i + 1
                    while j < len(stmts) and not uses(stmts[j], [n]):
                        j += 1
                    if j < len(stmts):
                        core = strip_comments(stmts[j]).strip()
                        am = re.match(r"^" + re.escape(n) + r"\s*=(?!=)\s*(.+);$", core, re.S)
                        if am and not uses(am.group(1), [n]) and i + 1 == j:
                            self.initdecls.append(dict(type=inf["type"], name=n, expr=am.group(1).strip(),
                                                       ws=leading_ws(st), default_split=True,
                                                       orig_anchor=len(self.F)))
                            self.F.append(("init", len(self.initdecls) - 1))
                            i = j + 1
                            continue
                self.movable.append(dict(text=st.strip(), names=inf["names"], orig_anchor=len(self.F),
                                         ws=leading_ws(st)))
                i += 1
                continue
            if inf and inf["scalar"] and inf["init"] is not None and not inf["const"] and len(inf["names"]) == 1:
                self.initdecls.append(dict(type=inf["type"], name=inf["names"][0], expr=inf["init"].strip(),
                                           ws=leading_ws(st), default_split=False, orig_anchor=len(self.F)))
                self.F.append(("init", len(self.initdecls) - 1))
                i += 1
                continue
            blocks = brace_blocks(st)
            if blocks:
                pieces, subs, pos = [], [], 0
                for (o, c) in blocks:
                    pieces.append(st[pos:o + 1])
                    subs.append(Model(st[o + 1:c], None, do_swap, self.known_numeric))
                    pos = c
                pieces.append(st[pos:])
                if all(s.ok for s in subs):
                    self.subs[len(self.F)] = (pieces, subs)
            self.F.append(st)
            i += 1
        # first-use index for movables
        for mv in self.movable:
            fu = len(self.F)
            for k, f in enumerate(self.F):
                txt = self.fixed_text(k, None)
                if uses(txt, mv["names"]):
                    fu = k; break
            mv["firstuse"] = fu
            if mv["orig_anchor"] > fu:
                # original placement after the first use?! (e.g. use inside a comment) -> pin it
                mv["firstuse"] = mv["orig_anchor"]
        # swap sites: in fixed statements and init exprs
        self.swaps = []  # (kind, index, span_start, span_end, a, op, b)
        if do_swap and self.known_numeric:
            for k, f in enumerate(self.F):
                if k in self.subs:
                    continue   # sub-models handle their own bodies
                if isinstance(f, tuple):
                    txt = self.initdecls[f[1]]["expr"]
                    kind = ("init", f[1])
                else:
                    txt = f
                    kind = ("F", k)
                for sm in re.finditer(r"(?<![\w.>\]])(\w+(?:\.\d*f?)?)\s*([*+])\s*(\w+(?:\.\d*f?)?)(?![\w(\[])", txt):
                    a, op, b = sm.group(1), sm.group(2), sm.group(3)
                    def okname(x):
                        return x in self.known_numeric or re.match(r"^\d+(\.\d*)?f?$", x)
                    if not (okname(a) and okname(b)):
                        continue
                    if not (a in self.known_numeric or b in self.known_numeric):
                        continue
                    pre = txt[:sm.start()].rstrip()
                    post = txt[sm.end():].lstrip()
                    bad = "+-*/%" if op == "+" else "*/%"
                    if (pre and pre[-1] in bad) or (post and post[0] in bad) or (pre and pre[-1] == "-" and op == "*"):
                        continue
                    if pre and pre[-1] in "<>" and op == "*":
                        continue
                    if skip_token(txt, sm.start()) != sm.start():
                        continue
                    self.swaps.append((kind, sm.start(), sm.end(), a, op, b))

    def fixed_text(self, k, cand):
        f = self.F[k]
        if isinstance(f, tuple):
            d = self.initdecls[f[1]]
            split = d["default_split"] if cand is None else cand["split"][f[1]]
            expr = self.swapped_text(("init", f[1]), d["expr"], cand)
            if split:
                adjacent = cand is not None and cand["ianchor"][f[1]] == d["orig_anchor"]
                ws = ("\n" + d["ws"].split("\n")[-1]) if adjacent else d["ws"]
                return ws + d["name"] + " = " + expr + ";"
            return d["ws"] + d["type"] + " " + d["name"] + " = " + expr + ";"
        if k in self.subs:
            pieces, subs = self.subs[k]
            out = pieces[0]
            for j, sub in enumerate(subs):
                sc = None if cand is None else cand["sub"][(k, j)]
                out += sub.render(sc if sc is not None else sub.base_candidate()) + pieces[j + 1]
            return out
        return self.swapped_text(("F", k), f, cand)

    def sub_items(self):
        return [((k, j), sub) for k in sorted(self.subs) for j, sub in enumerate(self.subs[k][1])]

    def n_knobs(self):
        return (len(self.movable) + len(self.initdecls) + len(self.swaps)
                + sum(sub.n_knobs() for _, sub in self.sub_items()))

    def copy_cand(self, cand):
        return dict(split=list(cand["split"]), anchor=list(cand["anchor"]), ianchor=list(cand["ianchor"]),
                    order=list(cand["order"]), swap=list(cand["swap"]),
                    sub=dict((key, sub.copy_cand(cand["sub"][key])) for key, sub in self.sub_items()))

    def swapped_text(self, kind, txt, cand):
        if cand is None:
            return txt
        out = txt
        for si in sorted(range(len(self.swaps)), key=lambda x: -self.swaps[x][1]):
            sw = self.swaps[si]
            if sw[0] != kind or not cand["swap"][si]:
                continue
            _, s0, s1, a, op, b = sw
            out = out[:s0] + b + " " + op + " " + a + out[s1:]
        return out

    def base_candidate(self):
        return dict(split=[d["default_split"] for d in self.initdecls],
                    anchor=[mv["orig_anchor"] for mv in self.movable],
                    ianchor=[d["orig_anchor"] for d in self.initdecls],
                    order=list(range(len(self.movable) + len(self.initdecls))),
                    swap=[False] * len(self.swaps),
                    sub=dict((key, sub.base_candidate()) for key, sub in self.sub_items()))

    def render(self, cand):
        """Movables are ids 0..m-1, split init decls ids m..m+n-1 (in order list)."""
        m = len(self.movable)
        at = {}
        def ws_for(ws, moved):
            # a moved declaration takes a plain newline + indent (no blank lines / comments travel with it)
            return ("\n" + ws.split("\n")[-1]) if moved else ws
        for idx in cand["order"]:
            if idx < m:
                mv = self.movable[idx]
                a = cand["anchor"][idx]
                at.setdefault(a, []).append(ws_for(mv["ws"], a != mv["orig_anchor"]) + mv["text"])
            else:
                di = idx - m
                if cand["split"][di]:
                    d = self.initdecls[di]
                    a = cand["ianchor"][di]
                    at.setdefault(a, []).append(ws_for(d["ws"], a != d["orig_anchor"])
                                                + d["type"] + " " + d["name"] + ";")
        parts = []
        for k in range(len(self.F) + 1):
            parts.extend(at.get(k, []))
            if k < len(self.F):
                parts.append(self.fixed_text(k, cand))
        return "".join(parts) + self.tail

    def ranges(self):
        r = [range(mv["firstuse"] + 1) for mv in self.movable]
        ri = [range(d["orig_anchor"] + 1) for d in self.initdecls]
        return r, ri

    def space_size(self):
        r, ri = self.ranges()
        n = 1
        for x in r: n *= len(x)
        for x in ri: n *= len(x)
        n *= 2 ** len(self.initdecls) * 2 ** len(self.swaps)
        k = len(self.movable) + len(self.initdecls)
        for i in range(2, k + 1): n *= i
        for _, sub in self.sub_items():
            n *= sub.space_size()
        return n

    def enumerate_own(self):
        r, ri = self.ranges()
        k = len(self.movable) + len(self.initdecls)
        for order in itertools.permutations(range(k)):
            for anchors in itertools.product(*r):
                for ianchors in itertools.product(*ri):
                    for splits in itertools.product([False, True], repeat=len(self.initdecls)):
                        for swaps in itertools.product([False, True], repeat=len(self.swaps)):
                            yield dict(split=list(splits), anchor=list(anchors), ianchor=list(ianchors),
                                       order=list(order), swap=list(swaps), sub={})

    def enumerate_all(self):
        items = self.sub_items()
        sublists = [list(sub.enumerate_all()) for _, sub in items]
        for own in self.enumerate_own():
            for combo in itertools.product(*sublists):
                c = dict(own)
                c["sub"] = dict((items[i][0], combo[i]) for i in range(len(items)))
                yield c

    def structured(self):
        """A few shapes worth trying first."""
        base = self.base_candidate()
        r, ri = self.ranges()
        out = []
        for key, sub in self.sub_items():
            for sc in sub.structured():
                c = self.copy_cand(base); c["sub"][key] = sc; out.append(c)
        c = dict(base); c["anchor"] = [0] * len(r); c["ianchor"] = [0] * len(ri); c["split"] = [True] * len(ri); out.append(c)
        c = dict(base); c["anchor"] = [x[-1] for x in r]; out.append(c)
        c = dict(base); c["split"] = [True] * len(ri); c["ianchor"] = [x[-1] for x in ri]; out.append(c)
        c = dict(base); c["split"] = [False] * len(ri); out.append(c)
        c = dict(base); c["anchor"] = [0] * len(r); out.append(c)
        for i in range(len(self.swaps)):
            c = dict(base); c["swap"] = list(base["swap"]); c["swap"][i] = True; out.append(c)
        return out

    def perturb(self, cand, rng):
        c = self.copy_cand(cand)
        r, ri = self.ranges()
        own = len(self.movable) + len(self.initdecls) + len(self.swaps)
        items = self.sub_items()
        weights = [own] + [sub.n_knobs() for _, sub in items]
        if sum(weights) == 0:
            return c
        pick = rng.choices(range(len(weights)), weights=weights)[0]
        if pick > 0:
            key, sub = items[pick - 1]
            c["sub"][key] = sub.perturb(c["sub"][key], rng)
            return c
        for _ in range(rng.choice([1, 1, 2, 3])):
            ops = []
            if r: ops.append("anchor")
            if ri: ops += ["split", "ianchor"]
            if len(c["order"]) > 1: ops.append("order")
            if c["swap"]: ops.append("swap")
            if not ops:
                return c
            op = rng.choice(ops)
            if op == "anchor":
                i = rng.randrange(len(r)); c["anchor"][i] = rng.choice(r[i])
            elif op == "ianchor":
                i = rng.randrange(len(ri)); c["ianchor"][i] = rng.choice(ri[i]); c["split"][i] = True
            elif op == "split":
                i = rng.randrange(len(ri)); c["split"][i] = not c["split"][i]
            elif op == "order":
                i, j = rng.sample(range(len(c["order"])), 2)
                c["order"][i], c["order"][j] = c["order"][j], c["order"][i]
            elif op == "swap":
                i = rng.randrange(len(c["swap"])); c["swap"][i] = not c["swap"][i]
        return c

# ----------------------------------------------------------------------------- build / score

def run(args, **kw):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors="replace", **kw)


def unit_table(unit):
    out = run([PY, "tools/decomp-diff.py", "-u", "mario/" + unit]).stdout
    rows = {}
    for l in out.splitlines():
        m = re.match(r"\s*(\w+)\s+([\d.]+)%\s+(\d+)B\s+(\S+)\s+(.*)$", l)
        if m:
            rows[m.group(5).strip()] = float(m.group(2))
    return rows


def func_diff_lines(unit, sig):
    out = run([PY, "tools/decomp-diff.py", "-u", "mario/" + unit, "-d", sig, "--no-collapse"]).stdout
    pct, lines, inside = None, 0, False
    for l in out.splitlines():
        if l.startswith(sig + ":"):
            m = re.search(r"([\d.]+)% match", l)
            pct = float(m.group(1)) if m else None
            inside = True
            continue
        if inside and re.match(r"[^\s|]\S*:", l) and "% match" in l:
            inside = False
        if inside and l[:1] in "~<>|":
            lines += 1
    return pct, lines


def compile_obj(obj):
    r = run([PY, "-m", "ninja", obj])
    return r.returncode == 0, (r.stdout + r.stderr)[-600:]


def score(unit, sig, obj, base_rows):
    ok, log = compile_obj(obj)
    if not ok:
        ls = log.splitlines()
        errs = [i for i, l in enumerate(ls) if re.search(r"error", l, re.I) and "ninja" not in l]
        msg = " | ".join(x.strip() for x in ls[max(0, errs[0] - 2):errs[0] + 2]) if errs else "(see log)"
        return None, "compile error: " + msg[:240]
    rows = unit_table(unit)
    for k, v in base_rows.items():
        if k != sig and rows.get(k, 0.0) < v - 1e-9:
            return None, "regression in %s (%.1f -> %.1f)" % (k, v, rows.get(k, 0.0))
    pct, lines = func_diff_lines(unit, sig)
    if pct is None:
        return None, "function not found in diff output"
    return (pct, -lines), ""

# ----------------------------------------------------------------------------- driver

def search_one(unit, sig, max_compiles, rng, do_swap=True, dry=False):
    src = os.path.join(ROOT, "src", unit.replace("/", os.sep) + ".cpp")
    obj = "build\\GMSE01\\src\\" + unit.replace("/", "\\") + ".o"
    if not os.path.isfile(src):
        print("%s %s: no source file %s" % (unit, sig, src)); return None
    global ALLOW_NONASCII
    raw = open(src, "rb").read()
    try:
        raw.decode("utf-8"); ALLOW_NONASCII = True
    except UnicodeDecodeError:
        ALLOW_NONASCII = False
    text, regions = collapse_port(raw.decode("latin-1"))
    if expand_port(text, regions) != raw.decode("latin-1"):
        print("%s %s: port-region round trip failed, skipped" % (unit, sig)); return None
    t0 = time.time()
    try:
        b0, b1, params = locate_body(text, sig)
    except ValueError as e:
        print("%s %s: %s" % (unit, sig, e)); return None
    body = text[b0:b1]
    model = Model(body, params, do_swap)
    if not model.ok:
        print("%s %s: skipped (%s)" % (unit, sig, model.reason)); return None
    base = model.base_candidate()
    if model.render(base) != body:
        # only whitespace / join canonicalisation differences are expected; verify the compact forms agree
        if re.sub(r"\s+", "", model.render(base)) != re.sub(r"\s+", "", body):
            print("  render(base) != body, model rejected");
            if dry:
                print(model.render(base))
            return None
    def movable_names(m):   # only these declarations change position (statements are never reordered)
        s = set(d["name"] for d in m.initdecls)
        for mv in m.movable:
            s.update(mv["names"])
        for _, sub in m.sub_items():
            s |= movable_names(sub)
        return s
    local_names = movable_names(model)
    n_knobs = model.n_knobs()
    size = model.space_size()
    print("%s %s: %d knobs (%d movable, %d init-decls, %d swaps at top level, %d nested blocks), space %d" % (
        unit, sig, n_knobs, len(model.movable), len(model.initdecls), len(model.swaps), len(model.sub_items()), size))
    if dry:
        def dump(m, ind):
            for mv in m.movable: print(ind + "movable", mv["names"], "anchor", mv["orig_anchor"], "firstuse", mv["firstuse"])
            for d in m.initdecls: print(ind + "init", d["name"], "=", d["expr"][:40].replace("\n", " "), "split", d["default_split"])
            for sw in m.swaps: print(ind + "swap", sw[3], sw[4], sw[5])
            for key, sub in m.sub_items():
                print(ind + "block", key); dump(sub, ind + "   ")
        dump(model, "   ")
        return None
    if n_knobs == 0:
        print("  no knobs"); return None
    ok, log = compile_obj(obj)   # the object must reflect the current source
    if not ok:
        print("  baseline does not compile"); return None
    base_rows = unit_table(unit)
    base_score, _ = (func_diff_lines(unit, sig)), ""
    if base_score[0] is None:
        print("  function not found by decomp-diff (check the signature)"); return None
    base_score = (base_score[0], -base_score[1])
    best, best_cand, best_text = base_score, None, None
    seen = {hashlib.md5(body.encode("latin-1")).hexdigest()}
    compiles = 0
    gen = model.enumerate_all() if size <= max_compiles * 2 else None
    cands = list(model.structured())
    current = base
    try:
        while compiles < max_compiles:
            if cands:
                cand = cands.pop(0)
            elif gen is not None:
                try:
                    cand = next(gen)
                except StopIteration:
                    break
            else:
                cand = model.perturb(best_cand or base, rng)
            new_body = model.render(cand)
            h = hashlib.md5(new_body.encode("latin-1")).hexdigest()
            if h in seen:
                if gen is None and not cands:
                    # random mode: avoid spinning forever on a tiny space
                    for _ in range(50):
                        cand = model.perturb(best_cand or base, rng)
                        new_body = model.render(cand)
                        h = hashlib.md5(new_body.encode("latin-1")).hexdigest()
                        if h not in seen:
                            break
                    else:
                        break
                else:
                    continue
            seen.add(h)
            new_text = expand_port(text[:b0] + new_body + text[b1:], regions, body, new_body, local_names)
            if new_text is None:
                print("  candidate crosses a port region, skipped"); continue
            open(src, "wb").write(new_text.encode("latin-1"))
            compiles += 1
            sc, why = score(unit, sig, obj, base_rows)
            if sc is None:
                print("  #%d rejected: %s" % (compiles, why))
                continue
            tag = ""
            if sc > best:
                best, best_cand, best_text = sc, cand, new_text
                tag = "  <- best"
            print("  #%d %.1f%% %d lines%s" % (compiles, sc[0], -sc[1], tag))
            if sc[0] >= 100.0:
                break
    finally:
        if best_text is not None and best > base_score:
            open(src, "wb").write(best_text.encode("latin-1"))
        else:
            open(src, "wb").write(raw)
        compile_obj(obj)
    dt = time.time() - t0
    improved = best > base_score
    print("  result: %.1f%%/%d lines -> %.1f%%/%d lines, %d compiles, %.0fs%s" % (
        base_score[0], -base_score[1], best[0], -best[1], compiles, dt, "  KEPT" if improved else "  restored"))
    return dict(unit=unit, sig=sig, before=base_score, after=best, compiles=compiles, secs=dt, improved=improved)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--unit")
    ap.add_argument("--func")
    ap.add_argument("--list")
    ap.add_argument("--max-compiles", type=int, default=80)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-swap", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--redo", action="store_true", help="also search functions already in tools/regalloc_done.txt")
    a = ap.parse_args()
    done_path = os.path.join(ROOT, "tools", "regalloc_done.txt")
    done = set()
    if os.path.exists(done_path) and not a.redo:
        for l in open(done_path, encoding="utf-8"):
            p = l.rstrip("\n").split("\t")
            if len(p) >= 2:
                done.add((p[0], p[1]))
    jobs = []
    if a.list:
        for l in open(a.list, encoding="utf-8"):
            l = l.rstrip("\n")
            if not l.strip() or l.startswith("#"):
                continue
            u, f = l.split("\t", 1)
            jobs.append((u.strip(), f.strip()))
    if a.unit and a.func:
        jobs.append((a.unit, a.func))
    if not jobs:
        ap.error("give --unit/--func or --list")
    rng = random.Random(a.seed)
    results = []
    for u, f in jobs:
        if (u, f) in done:
            print("%s %s: already searched (tools/regalloc_done.txt; --redo to repeat)" % (u, f)); continue
        r = search_one(u, f, a.max_compiles, rng, not a.no_swap, a.dry_run)
        if r:
            results.append(r)
            if not a.dry_run:
                with open(done_path, "a", encoding="utf-8", newline="\n") as fh:
                    fh.write("%s\t%s\t%.1f/%d -> %.1f/%d\t%d compiles\t%s\n" % (
                        u, f, r["before"][0], -r["before"][1], r["after"][0], -r["after"][1], r["compiles"],
                        "KEPT" if r["improved"] else "no gain"))
    if len(results) > 1:
        print("\nSUMMARY: %d functions, %d improved, %d at 100%%" % (
            len(results), sum(r["improved"] for r in results), sum(r["after"][0] >= 100.0 for r in results)))
        for r in results:
            print("  %-28s %-60s %.1f/%d -> %.1f/%d (%d compiles, %.0fs)" % (
                r["unit"], r["sig"][:60], r["before"][0], -r["before"][1], r["after"][0], -r["after"][1], r["compiles"], r["secs"]))


if __name__ == "__main__":
    main()
