"""Run decomp-permuter on functions of a plain-C unit (the permuter cannot parse C++).

python tools/perm_run.py <src.c> <secs-per-function> <workdir> <fn> [fn ...]
Per function: <workdir>/<fn>/ with base.c (MWCC -E of the whole unit, so inlining context is real),
target.o (the original split object), compile.sh, settings.toml (objdump limited to the function).
Runs permuter.py -j 6 --best-only --stop-on-zero for the given time and prints the best score.
"""
import os, re, shlex, shutil, subprocess, sys, time, glob
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PERM = os.path.join(os.path.dirname(ROOT), "decomp-permuter")
PY = sys.executable
OBJDUMP = os.path.join(ROOT, "build", "binutils", "powerpc-eabi-objdump.exe").replace("\\", "/")

def compile_flags(src):
    rel = os.path.relpath(src, ROOT).replace("\\", "/")
    parts = rel.split("/")
    if parts[0] == "libs" and "src" in parts:  # libs/<lib>/src/<path> builds to build/GMSE01/src/<lib>/<path>
        rel = "src/" + parts[1] + "/" + "/".join(parts[parts.index("src") + 1:])
    obj = os.path.normpath(os.path.join("build", "GMSE01", os.path.splitext(rel)[0] + ".o"))
    cmd = subprocess.run([PY, "-m", "ninja", "-t", "commands", obj], cwd=ROOT, capture_output=True, text=True).stdout.strip().splitlines()[-1]
    args = shlex.split(cmd, posix=False)
    return args[2:args.index("-MMD")], obj

def main():
    src, secs, work, fns = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4:]
    flags, obj = compile_flags(os.path.abspath(src))
    target = os.path.join(ROOT, obj.replace(os.sep + "src" + os.sep, os.sep + "obj" + os.sep, 1).replace("build" + os.sep + "GMSE01" + os.sep + "src", "build" + os.sep + "GMSE01" + os.sep + "obj"))
    target = os.path.join(ROOT, "build", "GMSE01", "obj", os.path.relpath(obj, os.path.join("build", "GMSE01", "src")))
    pp = subprocess.run([os.path.join(ROOT, "build", "tools", "sjiswrap.exe"), os.path.join(ROOT, "build", "compilers", "GC", "1.2.5", "mwcceppc.exe")]
                        + [f.strip('"') for f in flags] + ["-E", os.path.relpath(src, ROOT)], cwd=ROOT, capture_output=True, text=True, errors="replace").stdout
    env = dict(os.environ)
    env["PATH"] = os.path.join(PERM, "bin") + os.pathsep + env["PATH"]
    flagstr = " ".join(flags)
    for fn in fns:
        d = os.path.join(work, fn)
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
        open(os.path.join(d, "base.c"), "w", encoding="utf-8").write(pp)
        shutil.copy(target, os.path.join(d, "target.o"))
        rootb = "/" + ROOT[0].lower() + ROOT[2:].replace("\\", "/")
        open(os.path.join(d, "compile.sh"), "w", newline="\n").write(
            "#!/bin/bash\ncd \"%s\"\n./build/compilers/GC/1.2.5/mwcceppc.exe %s -c \"$1\" -o \"$3\" >/dev/null\n" % (rootb, flagstr))
        open(os.path.join(d, "settings.toml"), "w").write(
            'func_name = "%s"\ncompiler_type = "mwcc"\nobjdump_command = "%s -dr -EB -mpowerpc -M broadway --disassemble=%s"\n' % (fn, OBJDUMP, fn))
        p = subprocess.Popen([PY, os.path.join(PERM, "permuter.py"), d, "-j", "6", "--best-only", "--stop-on-zero", "--quiet"],
                             cwd=PERM, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            p.wait(timeout=secs)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        outs = sorted(glob.glob(os.path.join(d, "output-*")), key=lambda x: int(os.path.basename(x).split("-")[1]))
        best = os.path.basename(outs[0]).split("-")[1] if outs else "none"
        print("%s best=%s" % (fn, best), flush=True)

if __name__ == "__main__":
    main()
