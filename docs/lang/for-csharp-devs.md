# Polyglot for C# developers

Polyglot (`.pg`) is one small source language that transpiles to **idiomatic, readable C# and
TypeScript and Python** — you write the program once, and each target gets code that looks
hand-written. The VS Code extension shows you the **generated C# live as you type** ("Show
Generated Output" opens the emitted C#/TS/Python beside your `.pg` file), so you never have to
guess what a construct becomes. This page is the cheat sheet: each `.pg` construct next to the
C# you already know. The authority is [`SPEC.md`](SPEC.md); runnable examples are in
[`samples/`](samples/).

## Variables

| Polyglot | C# |
|---|---|
| `let x = 41` | `var x = 41;` — **never reassignable** (think readonly local) |
| `let y: f64 = 1.5` | `double y = 1.5;` (immutable) |
| `var count = 0` | `var count = 0;` (mutable — reassignment allowed) |
| `const Width: f32 = 620f` | `const float Width = 620f;` (compile-time constant) |

Statements are newline-terminated; `;` is optional (only needed to put two statements on one line).

## Functions

| Polyglot | C# |
|---|---|
| `fn square(x: i32): i32 => x * x` | `int Square(int x) => x * x;` |
| `fn log(msg: string) { ... }` | `void Log(string msg) { ... }` (no `: T` = `void`) |
| `fn greet(name: string, greeting: string = "Hello"): string` | default parameter values, as in C# |
| `fn describe(x: i32): string` + `fn describe(x: f64): string` | overloading — emits real C# overloads |

Return type is `: T` **after** the parameter list — not `-> T`, and the type comes after the name
(`x: i32`, like TypeScript/Kotlin).

## Integer widths and floats

Widths map 1:1 by name — the whole C# numeric model carries over:

| Polyglot | C# | TS (for context) |
|---|---|---|
| `i8 i16 i32 i64` | `sbyte short int long` | `number` (masked) / `bigint` for `i64` |
| `u8 u16 u32 u64` | `byte ushort uint ulong` | `number` / `bigint` for `u64` |
| `f32` / `f64` | `float` / `double` | `number` |
| `42`, `42i64`, `255u8`, `2.0f` | literal suffixes; default int is `i32`, default float is `f64` | |

There is **no `decimal`** — refused by design (see "Scope by design" below).

## Casts and conversions

| Polyglot | C# |
|---|---|
| `(i32)someI64` | `(int)someLong` — narrows/wraps exactly like C# |
| `(i32)(-7.9)` → `-7` | truncates toward zero, exactly like C# |
| implicit lossless widening (`i32` → `i64`, `i32` → `f64`) | same as C# implicit conversions |
| `i32.parse(s)` / `f64.parse(s)` | `int.Parse(s)` — parsing is a static method, never a cast |

## Strings and interpolation

| Polyglot | C# |
|---|---|
| `"hi ${name}, ${age * 2} doubled"` | `$"hi {name}, {age * 2} doubled"` |
| `'A'` (char, UTF-16 code unit) | `'A'` |
| `"ab" + "c" == "abc"` → `true` | string `==` is value equality, as in C# |
| `"tab\there"`, `"\u{E9}"`, `'\u{41}'` | `"tab\there"`, `"é"`, `'A'` — code points are written `\u{1–6 hex}` |
| `"cost \$5"`, `"\${raw}"` | `\$` is a literal dollar, so `"\${raw}"` is the text `${raw}`, not a hole |

The escapes are `\n \t \r \0 \\ \" \' \$ \u{…}` — nothing else. C#'s `\uXXXX`, `\xHH` and `\U…` are errors
(the variable-length `\x` is a known C# trap), and a char literal is exactly one UTF-16 code unit, so
`'\u{1F600}'` is an error: use a string. Indexing/length count UTF-16 code units on C# and TS; on Python and
PHP they count in those targets' native units for non-ASCII text (SPEC §9 — the compiler warns).

**Number text is not `ToString()`.** A number in a hole or in `print` follows one rule on every target
(SPEC §8): integers as invariant decimal, `f64` as JavaScript prints it (`0.1 + 0.2` →
`0.30000000000000004`, `1e21` → `1e+21`, `1e-7` → `1e-7`, `-0.0` → `0`), `f32` as its exact value widened to
`f64` (`0.1f` → `0.10000000149011612`). The generated C# routes those holes through a file-local
`__PgRt.Str(…)`, so the current culture never matters and `Main` sets none. A record, list, map or **enum** in
a hole (or in `print`) is a compile error — C# would print `Point { X = 1 }` or the enum's case name, the
other targets something else — so format it explicitly. `toUpper()`/`toLower()` emit
`ToUpperInvariant()`/`ToLowerInvariant()` and are best-effort across targets; use `toUpperAscii()`/
`toLowerAscii()` when the result must be identical everywhere.

## Records (structural equality, `with`)

```pg
record Vec2(x: f64, y: f64) {
  operator fn plus(o: Vec2): Vec2 => Vec2(this.x + o.x, this.y + o.y)
}
let a = Vec2(1.0, 2.0)
let b = a with { x = 9.0 }          // Vec2(9.0, 2.0)
let same = a == Vec2(1.0, 2.0)      // true — structural
```

Maps almost 1:1 to a C# positional `record` with `with` and structural `==`. Differences:
`.pg` records are **always immutable** (no mutable value types — C# `struct` has no equivalent
and is refused), construction never uses `new`, and operators are **named methods**
(`operator fn plus`) that the C# backend turns into real `operator +` members.

## Enums and unions

| Polyglot | C# |
|---|---|
| `enum Direction { North, East, South, West }` | `enum` — constants only |
| `enum HttpCode { Ok = 200, NotFound = 404 }` | explicit values allowed |
| `union Shape { Circle(r: f64) Rect(w: f64, h: f64) Empty }` | abstract `record Shape` + sealed case records |

Data-carrying variants are a `union` (an ADT), never an enum. `match` over a union is
**exhaustiveness-checked at compile time** — a missing case is an error, unlike a C# `switch`
that silently falls through:

```pg
fn area(s: Shape): f64 => match s {
  Circle(r)  => Math.PI * r * r,
  Rect(w, h) => w * h,
  Empty      => 0.0,
}
fn classify(n: i32): string => match n {
  0          => "zero",
  x if x < 0 => "negative",     // guard, like `when` in a C# switch
  _          => "many",
}
```

`match` emits a C# `switch` expression with pattern matching.

## Classes, inheritance, interfaces

```pg
open class Shape { open fn area(): f64 => 0.0 }

class Disk : Shape {
  let r: f64
  init(r: f64) { this.r = r }          // constructor is `init`
  override fn area(): f64 => 3.141592653589793 * this.r * this.r
}

interface Comparable<T> { fn compareTo(other: T): i32; }
record Version(major: i32, minor: i32) : Comparable<Version> {
  override fn compareTo(o: Version): i32 => this.major - o.major
}
```

| Polyglot | C# |
|---|---|
| `init(...)` / `super(msg)` | constructor / `base(msg)` |
| `open class` / `open fn` | non-`sealed` class / `virtual` method — **sealed/non-virtual is the default** |
| `override` | `override` — but also **required when implementing an interface** (Kotlin's rule) |
| `class Disk : Shape, Disposable` | one base class + interfaces, all after `:` |
| `let area: f64 => expr` | read-only computed property (`double Area => expr;`) |
| `var diameter: f64 { get => ... set(v) { ... } }` | property with getter/setter |
| `operator fn get(x: i32, y: i32): T` | indexer `this[int x, int y]` — used as `grid[x, y]` |
| `static fn`, `const` members | `static` methods, `const` fields |

## Generics and bounds

| Polyglot | C# |
|---|---|
| `fn maxOf<T: Comparable<T>>(a: T, b: T): T` | `T MaxOf<T>(T a, T b) where T : Comparable<T>` |
| `<T: A & B>` | `where T : A, B` |
| `<T: INumber>` | like `where T : INumber<T>` — compile-time-only numeric bound, erased on emit |
| `class RingBuffer<T> { ... }`, `List<T>()` | generic classes; construction with explicit type args |

## Lambdas and closures

| Polyglot | C# |
|---|---|
| `x => x * 2` | `x => x * 2` — identical |
| `(a, b) => a + b`, `(x: i32) => x + 1` | parens required except for a bare single untyped param |
| `(n: i32) => { total += n }` | block body; captures by reference, like C# |
| `fn scaler(factor: i32): (i32) => i32` | function type `(i32) => i32` ≈ `Func<int, int>` |

Lambdas are always executable closures — there are no expression trees (`Expression<Func<...>>`
is refused), so no LINQ providers.

## Iterators, `for..in`, ranges

```pg
fn countUp(n: i32): Iterable<i32> {
  var i = 0
  while i < n { yield i; i += 1 }
}
for v in countUp(3) { print(v) }   // 0 1 2
for i in 0..n { ... }              // exclusive; 0..=n inclusive
for (a, b) in pairs { ... }        // tuple destructuring
```

`Iterable<T>` + `yield` → C# `IEnumerable<T>` + `yield return`; `for x in seq` → `foreach`;
integer ranges emit a plain C-style `for` loop.

## Exceptions

```pg
class ParseError : Error { init(msg: string) { super(msg) } }

try {
  let t = parseTier(input)
} catch (e: ParseError) when (input.length < 3) {
  print("short bad input")
} catch (e: Error) {
  print("other: ${e.message}")
} finally { cleanup() }
```

`Error` is the root exception type (→ `System.Exception`); `throw ParseError("...")` needs no
`new`; typed `catch` + `when` filters + `finally` map directly to C#.

## Disposal (`use`)

```pg
use file = openFile(path) {
  file.writeLine(data)
}   // file.dispose() runs at block end, even on throw
```

≈ C# `using var file = openFile(path);` scoped to the block. The value must implement
`Disposable` (`fn dispose()`). There are **no finalizers** — `use` is the entire disposal story.

## Async/await

```pg
async fn doubleIt(x: i32): i32 { return x + x }
async fn main() { print(await doubleIt(10)) }
```

You write the **unwrapped** return type (`: i32`, not `Task<i32>`); the C# backend synthesizes
`async Task<int>`. Single-threaded async only — no `Task.Run`, no threads.

## Nullability

| Polyglot | C# |
|---|---|
| `let maybe: i32? = null` | `int? maybe = null;` — non-null by default |
| `name?.length`, `maybe ?? 0` | `?.` and `??`, as in C# |
| `maybe!` | **throws if null at runtime** — unlike C# `!`, which is compile-time-only. Emits `(maybe ?? throw new NullReferenceException("null-assert failed (m.pg:L:C)"))`, so `int?` yields `int` |
| `maybe + 1`, `"${maybe}"`, `print(maybe)`, `maybe.x` | **errors** — C# would null-lift / print `""`; narrow, `??` or `!` first |
| `if x != null { x + 1 }` | flow-narrowing like C# NRT, but for locals and parameters only, and it ends at any assignment to `x` |
| `if c { 1 } else { null }` | typed `i32?` |

## Extension methods

```pg
extension fn string.shout(): string => this.toUpper() + "!"
extension fn List<T>.secondOrNull(): T? => if this.count >= 2 { this[1] } else { null }
```

→ C# static extension methods (`this string` parameter). Call sites read the same: `"hi".shout()`.

## Imports and modules

```pg
import { print } from "std.io"
import { Math } from "std.math"
import { List } from "std.collections"
import { readText as rt } from "std.io"   // `as` rebinds
import * as coll from "std.collections"
import "./physics"                        // relative user module
```

TypeScript-style imports, not `using` directives. One `.pg` file is one module; top-level
declarations are module-public.

There is no `namespace` statement in `.pg` source: where the emitted C# lives is build configuration,
not language. Top-level functions and globals land in a static wrapper class (`PolyglotProgram` by
default), and the generated types are `internal` unless you ask otherwise. For a library, set the C#
target options in `pgconfig.json` (or the matching MSBuild properties `PolyglotNamespace` /
`PolyglotClass` / `PolyglotAccess`):

```json
{ "targets": ["csharp"],
  "targetOptions": { "csharp": { "namespace": "MyCompany.Strings", "class": "StringsApi", "access": "public" } } }
```

The emitted file then starts `namespace MyCompany.Strings;`, callers write `StringsApi.trim(...)`, and two
libraries built from `.pg` can be referenced by one app without CS0433 clashes. Runtime helpers such as
`print` stay `internal` even when `access` is `public`.

## False friends — where your C# intuition misleads

- **`let` is not C# `var`.** `let` is immutable (closer to a readonly local); mutable locals are
  `var`. Reassigning a `let` is a compile error.
- **`x!` throws.** C#'s null-forgiving `!` is erased at compile time; `.pg`'s `!` is a runtime
  null-assert that throws when the value is null.
- **`override` is required for interface implementations**, not just virtual overrides.
- **No `new`.** `Vec2(1.0, 2.0)`, `List<i32>()`, `ParseError("msg")` — construction is a plain call.
- **`match` must be exhaustive** over a union; there is no default fall-through unless you write `_`.
- **Operators are named methods** (`operator fn plus/minus/times/eq/lt/...`); the C# backend emits
  the real symbol operators for you.
- **No mutable value types.** There is no `struct`; immutable data is a `record`, mutable data is a
  `class` (reference identity).
- **`"${x}"` is not `x.ToString()`.** A double prints the JavaScript way (`1e+21`, never `1E+21`), an
  enum cannot be interpolated at all, and the result never depends on `CultureInfo.CurrentCulture`.

## Scope by design (what Polyglot refuses)

To stay faithful on every target, Polyglot **refuses** (with a clear diagnostic, never a
miscompile): threads/`lock`/`Parallel`, runtime reflection, finalizers/GC hooks, `decimal`,
`unsafe`/pointers, `dynamic`/runtime code-gen, and LINQ expression trees. Cross-target floats are
bit-exact only for `+ − × ÷ √` on `f64` — transcendentals are not promised identical. See the
PRD's §3 support/refuse contract.
