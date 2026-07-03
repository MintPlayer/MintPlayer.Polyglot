# Polyglot for TypeScript developers

Polyglot (`.pg`) is one small source language that transpiles to **idiomatic, readable
TypeScript and C# and Python** — write the program once, and each target gets code that looks
hand-written. The surface is deliberately TS-flavored (`x: i32` annotations, `=>` arrows,
`${}` interpolation, TS-style imports), and the VS Code extension shows the **generated
TypeScript live as you type** ("Show Generated Output" opens the emitted TS/C#/Python beside
your `.pg` file). But a few familiar-looking spellings mean something *different* here — read
the [false friends](#false-friends--where-your-ts-intuition-misleads) section before writing
code. The authority is [`SPEC.md`](SPEC.md); runnable examples are in [`samples/`](samples/).

## Variables — the #1 false friend

| Polyglot | TypeScript |
|---|---|
| `let x = 41` | `const x = 41;` — **`let` is IMMUTABLE in .pg** |
| `var count = 0` | `let count = 0;` — **`.pg` `var` is the mutable one** |
| `const Width: f32 = 620f` | compile-time constant |

Yes, really: `.pg` `let` ≈ TS `const`, `.pg` `var` ≈ TS `let`. Statements are
newline-terminated; `;` is optional.

## Functions

| Polyglot | TypeScript |
|---|---|
| `fn square(x: i32): i32 => x * x` | `function square(x: number): number { return x * x; }` |
| `fn log(msg: string) { ... }` | `function log(msg: string): void { ... }` |
| `fn greet(name: string, greeting: string = "Hello"): string` | default parameter values, as in TS |
| `fn describe(x: i32): string` + `fn describe(x: f64): string` | **real overloading** with separate bodies (TS gets name-mangled functions) |

Same postfix `: T` types as TS; the keyword is `fn` and expression bodies use `=>`.

## Numbers — explicit widths, not one `number`

| Polyglot | TypeScript output |
|---|---|
| `i8 i16 i32 u8 u16 u32` | `number`, **masked at op boundaries** (`\| 0`, `>>> 0`, `Math.imul`) so it wraps like real fixed-width ints |
| `i64 u64` (`42i64`) | **`bigint`** — full 64-bit integers, no 2^53 precision loss |
| `f32` / `f64` (`2.0f` / `3.14`) | `number` |
| default int literal | `i32`; default float literal is `f64` |

You must pick a width; arithmetic behaves like that width on *every* target (this is the point
of the language). There is no `decimal` and no `any`.

## Casts — `(T)x` actually converts

| Polyglot | TypeScript |
|---|---|
| `(i32)someI64` | a real runtime conversion (`Number(big)` + masking) — **not** TS `as`, which does nothing |
| `(i32)(-7.9)` → `-7` | `Math.trunc`-style truncation toward zero |
| `(u8)300` → `44` | wraps/narrows like a CPU would |
| `i32.parse(s)` / `f64.parse(s)` | string→number parsing is a throwing static method, never a cast |

Lossless widenings (`i32` → `i64`, `i32` → `f64`) are implicit; everything else needs the cast.

## Strings and interpolation

| Polyglot | TypeScript |
|---|---|
| `"hi ${name}, ${age * 2} doubled"` | `` `hi ${name}, ${age * 2} doubled` `` — same spelling |
| `'A'` | a `char` — one UTF-16 code unit (a `number` in TS), not a 1-char string |
| `"ab" + "c" == "abc"` → `true` | `===` value equality |

Strings are UTF-16 on all targets, same as JS.

## Records — immutable data with structural `==`

```pg
record Vec2(x: f64, y: f64) {
  operator fn plus(o: Vec2): Vec2 => Vec2(this.x + o.x, this.y + o.y)
}
let a = Vec2(3.0, 4.0)
let b = a with { x = 0.0 }          // copy with one field changed
print(a == Vec2(3.0, 4.0))          // true — structural, by field
print((a + b).x)                    // TS output: a.plus(b).x
```

→ a TS `class` with `readonly` fields plus generated `equals()` and `with()`. Note: `==` on
records compares **fields**, not references — nothing like JS `===` on objects. Mutable data is
a `class` (reference identity, like JS objects).

## Enums and unions

| Polyglot | TypeScript |
|---|---|
| `enum Direction { North, East, South, West }` | `enum` / const-union of names |
| `union Shape { Circle(r: f64) Rect(w: f64, h: f64) Empty }` | tagged union `{ tag: "Circle", r: number } \| ...` |

A `.pg` `union` looks like a TS union type but is **nominal, closed, and
exhaustiveness-checked**: `match` must cover every case or it's a compile error.

```pg
fn area(s: Shape): f64 => match s {
  Circle(r)  => Math.PI * r * r,
  Rect(w, h) => w * h,
  Empty      => 0.0,
}
fn classify(n: i32): string => match n {
  0          => "zero",
  x if x < 0 => "negative",     // guard
  _          => "many",
}
```

→ `switch (s.tag)` with payload destructuring. `match` is an expression (so is `if`).

## Classes, inheritance, interfaces

```pg
open class Shape { open fn area(): f64 => 0.0 }

class Disk : Shape {
  let r: f64
  init(r: f64) { this.r = r }          // constructor
  override fn area(): f64 => 3.141592653589793 * this.r * this.r
}

interface Comparable<T> { fn compareTo(other: T): i32; }
record Version(major: i32, minor: i32) : Comparable<Version> {
  override fn compareTo(o: Version): i32 => this.major - o.major
}
```

| Polyglot | TypeScript |
|---|---|
| `init(...)` / `super(msg)` | `constructor(...)` / `super(msg)` |
| `class Disk : Shape, Disposable` | `extends` + `implements`, all after one `:` |
| `open` / `override` | methods are non-overridable unless `open`; `override` also required for interface methods |
| `let area: f64 => expr` | a `get area()` accessor (read-only computed property) |
| `var diameter: f64 { get => ... set(v) { ... } }` | `get`/`set` pair |
| `operator fn get(x: i32, y: i32): T` | indexer — `grid[x, y]` becomes `.get(x, y)`/`.set(x, y, v)` calls |
| interfaces are **nominal** | you must declare `: Comparable<Version>`; structural conformance isn't enough |

## Generics and bounds

| Polyglot | TypeScript |
|---|---|
| `fn maxOf<T: Comparable<T>>(a: T, b: T): T` | `<T extends Comparable<T>>` |
| `<T: A & B>` | `<T extends A & B>` |
| `<T: INumber>` | compile-time numeric-only bound (any of `i8..u64`, `f32`, `f64`); erased on emit |
| `RingBuffer<string>(2)`, `List<i32>()` | explicit construction type args |

## Lambdas and closures

| Polyglot | TypeScript |
|---|---|
| `x => x * 2` | `x => x * 2` — identical arrow spelling |
| `(a, b) => a + b`, `(x: i32) => x + 1` | parens required except for one bare untyped param |
| `(n: i32) => { total += n }` | block body; closes over `total` by reference |
| `fn scaler(factor: i32): (i32) => i32` | function type, ≈ `(x: number) => number` |

## Iterators, `for..in`, ranges

```pg
fn fibonacci(count: i32): Iterable<i64> {
  var a = 0i64
  var b = 1i64
  var n = 0
  while n < count { yield a; let next = a + b; a = b; b = next; n += 1 }
}
for f in fibonacci(8) { print(f) }
for i in 0..n { ... }              // exclusive; 0..=n inclusive
for (a, b) in pairs { ... }        // tuple destructuring
```

An iterator fn returns `Iterable<T>` and emits a TS `function*`. **`.pg` `for x in seq`
iterates VALUES** — it lowers to TS `for...of`, never JS `for...in` (which iterates keys).
Integer ranges become plain C-style `for` loops.

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

`Error` is the root type (→ JS `Error`); `throw ParseError("...")` needs no `new`. **Typed
catch** lowers to `instanceof` dispatch (unmatched types rethrow); `when` becomes a guard.
You can't throw non-Error values.

## Disposal (`use`)

```pg
use file = openFile(path) {
  file.writeLine(data)
}   // file.dispose() runs at block end, even on throw
```

→ TS `try { ... } finally { file[Symbol.dispose]() }` (the semantics of TS 5.2 `using`). The
value must implement `Disposable` (`fn dispose()`). No finalizers exist.

## Async/await

```pg
async fn doubleIt(x: i32): i32 { return x + x }
async fn main() { print(await doubleIt(10)) }
```

You write the **unwrapped** type (`: i32`, not `Promise<i32>`); the TS backend synthesizes
`async ... : Promise<number>`. Forgetting `await` on an async call is a **compile error** —
no accidentally-floating promises.

## Nullability

| Polyglot | TypeScript |
|---|---|
| `let maybe: i32? = null` | `number \| null` — non-null by default |
| `name?.length`, `maybe ?? 0` | `?.` and `??`, same as TS |
| `maybe!` | **runtime null-assert that throws** — TS `!` is erased and can lie |
| no `undefined` | `null`/`undefined` are normalized to a single `null`; `undefined` never leaks |

## Extension methods

```pg
extension fn string.shout(): string => this.toUpper() + "!"
extension fn List<T>.secondOrNull(): T? => if this.count >= 2 { this[1] } else { null }
// "hi".shout()  →  TS: shout("hi")
```

Call sites use method syntax; the TS backend emits free functions (no prototype patching).

## Imports and modules

```pg
import { print } from "std.io"
import { List } from "std.collections"
import { readText as rt } from "std.io"   // `as` rebinds
import * as coll from "std.collections"
import "./physics"                        // relative user module
```

Exactly TS import syntax — `.pg` copied it. `"std.x"` is a logical std module; `"./x"` is
importer-relative. One file = one module.

## False friends — where your TS intuition misleads

- **`let` is immutable.** `.pg` `let` ≈ TS `const`; use `.pg` `var` for a mutable binding.
- **There is no one `number`.** You choose a width (`i32`, `f64`, ...) and the semantics follow
  it faithfully — `i64` math is `bigint`, `i32` math wraps at 32 bits.
- **`(T)x` converts at runtime; TS `as` never does.** A `.pg` cast truncates, wraps, and
  changes representation — it is a computation, not an annotation.
- **`for x in xs` iterates VALUES** (TS `for...of`), never keys.
- **`==` on records is structural** — it compares fields, unlike `===` on JS objects.
- **`union` is closed and nominal** with compiler-enforced exhaustive `match` — not a
  structural `A | B` type.
- **`x!` throws at runtime** when null; TS `!` is a compile-time promise.
- **Interfaces are nominal** — you must declare that you implement them, and mark the methods
  `override`.

## Scope by design (what Polyglot refuses)

To stay faithful on every target, Polyglot **refuses** (with a clear diagnostic, never a
miscompile): threads/locks, runtime reflection, finalizers/GC hooks, `decimal`,
`unsafe`/pointers, `dynamic`/runtime code-gen, and LINQ-style expression trees. Cross-target
floats are bit-exact only for `+ − × ÷ √` on `f64` — transcendentals are not promised
identical. See the PRD's §3 support/refuse contract.
