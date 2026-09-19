class Direction:
    North = 0
    East = 1
    South = 2
    West = 3
# union Shape -> tagged dicts: {"tag": <case>, <field>: <value>, ...}
# union Option -> tagged dicts: {"tag": <case>, <field>: <value>, ...}
def area(s):
    return (lambda _m: (lambda r: __import__('math').pi * r * r)(_m["r"]) if _m["tag"] == "Circle" else (lambda w, h: w * h)(_m["w"], _m["h"]) if _m["tag"] == "Rect" else (lambda b, h: 0.5 * b * h)(_m["base"], _m["height"]) if _m["tag"] == "Triangle" else 0.0)(s)
def classify(n):
    return (lambda _m: "zero" if _m == 0 else (lambda x: "negative")(_m) if (lambda x: x < 0)(_m) else "one" if _m == 1 else "many")(n)
def turnRight(d):
    return (lambda _m: Direction.East if _m == Direction.North else Direction.South if _m == Direction.East else Direction.West if _m == Direction.South else Direction.North)(d)
def main():
    shapes = [{"tag": "Circle", "r": 2.0}, {"tag": "Rect", "w": 3.0, "h": 4.0}, {"tag": "Triangle", "base": 6.0, "height": 1.0}, {"tag": "Empty"}]
    for s in shapes:
        print(area(s))
    print(classify(((((-5) & 0xffffffff) ^ 0x80000000) - 0x80000000)))
    print(classify(1))
    print(classify(99))
    print(turnRight(Direction.North) == Direction.East)
def print(x):
    __builtins__.print(("true" if x else "false") if isinstance(x, bool) else (str(int(x)) if isinstance(x, float) and x.is_integer() and abs(x) < 1e16 else x))
def readText(path):
    return __import__('pathlib').Path(path).read_text()
def writeText(path, content):
    __import__('pathlib').Path(path).write_text(content)
def appendText(path, content):
    open(path, 'a').write(content)
def fileExists(path):
    return __import__('os').path.exists(path)
def deleteFile(path):
    __import__('os').remove(path)
main()
# sourceMappingURL=03_enums_unions_match.py.map
