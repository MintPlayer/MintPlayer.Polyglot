# P40 A3 corpus runner (Python). Executes the python plugin's SHIPPED `fstr` prelude (`_pg_str`) from the
# manifest and checks it against the ECMAScript reference text. Args: <manifest> <corpus>.
import json
import struct
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    spec = json.load(fh)["spec"]
preludes, requires = spec["preludes"], spec.get("preludeRequires", {})
order = []
def need(name):  # the emitter's rule: a prelude's `preludeRequires` come first (e.g. `import builtins`)
    if name in order:
        return
    for dep in requires.get(name, []):
        need(dep)
    order.append(name)
need("fstr")
scope = {}
exec("\n".join(preludes[n] for n in order), scope)
pg_str = scope["_pg_str"]

n = bad = 0
with open(sys.argv[2], encoding="utf-8") as fh:
    for line in fh:
        line = line.rstrip("\n")
        if not line:
            continue
        hexbits, want = line.split(" ", 1)
        x = struct.unpack(">d", bytes.fromhex(hexbits))[0]
        got = pg_str(x)
        n += 1
        if got != want:
            bad += 1
            if bad <= 10:
                print("  Python mismatch %s: want %s got %s" % (hexbits, want, got))
print("Python : %d doubles, %d mismatches" % (n, bad))
sys.exit(1 if bad else 0)
