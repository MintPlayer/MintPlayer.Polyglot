# MintPlayer.Polyglot — Language Specification (v0.1)

> **Status:** Draft v0.1 · 2026-06-28 · P1 deliverable (design-on-paper, no compiler yet).
> Companion to the formal grammar in [`grammar.ebnf`](grammar.ebnf) and the sample programs in
> [`samples/`](samples/). The PRD's §3 support/refuse contract ([`../prd/POLYGLOT_PRD.md`](../prd/POLYGLOT_PRD.md))
> is the law this language is shaped to; every feature below names how it lowers to **both** C# and
> TypeScript, and every §3.B refusal is *absent from the grammar entirely* rather than parsed-then-rejected.

This document is meant to be **reviewed and argued with**, not treated as final. Where a decision could
have gone another way, the rationale is stated so the alternative can be reconsidered (PRD §4.3 ethos).

---

## 1. Design goals for the surface syntax

1. **Lower cleanly to both targets.** Every construct must have an obvious, idiomatic emission in *both*
   C# and TypeScript. If a feature is natural in one and tortured in the other, it does not belong in v0.1.
   This single rule kills more would-be features than any other.
2. **One way to say each thing.** The transpilers that died drowned in surface area. v0.1 picks *one*
   spelling per concept (one loop-over-sequence form, one nullable form, one data-type form per mutability).
3. **Modern, postfix-typed, brace-delimited.** `name: Type`, `{ }` blocks. Familiar to anyone who knows
   TypeScript, Kotlin, Swift, or Rust; trivially printable to C# and TS.
4. **Faithful-by-default, never silently relaxed.** Where the two runtimes differ (§3.C), the default is
   the faithful behaviour and the relaxation is opt-in and documented (§9).
5. **The refused list is unspeakable.** No `lock`, `unsafe`, pointer types, `decimal`, `dynamic`, or
   finalizers exist in the grammar. You cannot write them, so you cannot be miscompiled by them (§10).

---

## 2. Lexical structure

| Element | Form | Notes |
|---|---|---|
| Line comment | `// …` | trivia — kept by the lexer for readable output (P2) |
| Block comment | `/* … */` | nesting allowed |
| Identifier | `[A-Za-z_][A-Za-z0-9_]*` | |
| Int literal | `42`, `0xFF`, `0b1010`, `1_000`, `42i64`, `255u8` | default type `i32`; `_` separators allowed |
| Float literal | `3.14`, `1e-9`, `2.0f`, `64f` | default type `f64`; suffix `f`/`f32` = `f32`, `d`/`f64` = `f64` |
| Bool | `true`, `false` | |
| Char | `'a'`, `'\n'`, `'\u{1F600}'` | a UTF-16 scalar; see §8 |
| String | `"…"`, `"hi ${name}!"` | interpolation with `${ expr }`; UTF-16 (§8) |
| Null | `null` | only assignable to a nullable type `T?` (§4.3) |

Statements are **newline-terminated**; `;` is an **optional separator**, needed only to place several
statements on one line (`a = 1; b = 2`). The sample programs omit it. Blocks `{ }` need no trailing `;`.
The lexer is trivia-bearing (it keeps comments and spacing) so backends can reproduce author intent in
the output.

---

## 3. Primitive types

| Polyglot | C# | TypeScript | Notes |
|---|---|---|---|
| `i8 i16 i32 i64` | `sbyte short int long` | `number` / `bigint` | `i64` → **`bigint`** in TS (§9); `i32` masked (§9) |
| `u8 u16 u32 u64` | `byte ushort uint ulong` | `number` / `bigint` | `u64` → `bigint` in TS |
| `f32` | `float` | `number` | strict single-precision is opt-in (§9); see §3.D of the PRD |
| `f64` | `double` | `number` | the default float |
| `bool` | `bool` | `boolean` | |
| `char` | `char` | `number` (UTF-16 code unit) | a single UTF-16 code unit (§8) |
| `string` | `string` | `string` | UTF-16 on both targets (§8) |
| `unit` | `void` | `void` | the "no meaningful value" type; a function with no `: T` returns `unit` |

There is **no `decimal`** and **no machine-pointer type** — both are on the refused list (§10). Integer
default is `i32`; float-literal default is `f64`. Mixed-width arithmetic requires an explicit conversion
(no implicit narrowing); widening conversions are explicit too in v0.1 for clarity (`x.toI64()`), revisited
if it proves noisy.

---

## 4. Declarations & types

### 4.1 Variables and constants

```pg
let x = 41          // immutable binding, type inferred (i32)
let y: f64 = 1.5    // immutable, explicit type
var count = 0       // mutable
const Width: f32 = 620f   // compile-time constant (module- or type-level)
```

`let` → cannot be reassigned (C# `var` + no reassignment / TS `const`); `var` → mutable (C# `var` /
TS `let`). `const` is a true compile-time scalar constant.

`let` and `const` may also appear at **module scope** (top-level). A top-level `const` is a compile-time
scalar; a top-level `let` is an immutable binding initialized at module-init time and may hold an aggregate
(→ C# `static readonly` / TS module-level `const`). A **list literal** `[a, b, c]` builds a `List<T>`
(→ C# collection expression `[…]` / TS array `[…]`):

```pg
let catalog: List<i32> = [ 16, 22, 30, 40 ]   // module-level immutable list
```

### 4.2 The two aggregate kinds — `class` and `record`

v0.1 deliberately offers exactly two ways to group data, split by **mutability**, because that split is
what makes value-vs-reference identity *unobservable across targets*:

| Kind | Semantics | Equality | C# | TypeScript |
|---|---|---|---|---|
| `class` | reference identity, **may be mutable** | reference (`==` is identity) | `class` | `class` |
| `record` | **immutable** value data | **structural** (`==` compares fields) | `record` | `class` w/ `readonly` fields + generated `equals()`/`with()` |

> **Why only these two (the key design call):** A *mutable value type* (C# `struct`) is the one construct
> that genuinely diverges — C# copies it on assignment, TS would alias it. So v0.1 **refuses mutable value
> types**. A `record` is immutable, which makes "did we copy or alias?" *unobservable* — TS can back it
> with a shared reference and still behave like a C# value, because nothing can mutate it. Mutable data is
> a `class` (reference identity, which both targets express identically). This is the cleanest scope line
> that keeps equality and identity faithful on both sides. `struct` may return post-v0.1 if a real need
> appears; it is intentionally out now.

```pg
class Account {            // mutable, reference identity
  let id: string           // set once in init
  var balance: f64

  init(id: string, balance: f64) {
    this.id = id
    this.balance = balance
  }

  fn deposit(amount: f64) { this.balance += amount }
}

record Vec2(x: f64, y: f64) {       // immutable, structural equality
  operator fn plus(o: Vec2): Vec2 => Vec2(this.x + o.x, this.y + o.y)
}

let a = Vec2(1.0, 2.0)
let b = a with { x = 9.0 }          // copy with one field changed -> Vec2(9.0, 2.0)
let same = (a == Vec2(1.0, 2.0))    // true: structural equality
```

`record` copies use a `with { field = … }` expression (C# `with`; TS a generated `with()` method).

### 4.3 Nullability

Types are **non-null by default**. A trailing `?` makes a type nullable; `null` is only assignable there.

```pg
let maybe: i32? = null
let n = maybe ?? 0          // ?? coalescing  (C# ??, TS ??)
let len = name?.length      // ?. null-safe access (C# ?., TS ?.)
let forced = maybe!         // ! null-assert: T? -> T (throws if null)
```

The compiler normalizes `null`/`undefined` to a single notion of "absent" (PRD §3.C): the TS backend
emits and tests against `null` consistently and never lets `undefined` leak into Polyglot semantics.

### 4.4 Interfaces, inheritance, generics

```pg
interface Comparable<T> { fn compareTo(other: T): i32; }

open class Shape { open fn area(): f64 => 0.0 }
class Disk : Shape { 
  let r: f64
  init(r: f64) { this.r = r }
  override fn area(): f64 => 3.141592653589793 * this.r * this.r
}

fn maxOf<T: Comparable<T>>(a: T, b: T): T => if a.compareTo(b) >= 0 { a } else { b }
```

A class has **at most one base class** and any number of interfaces, all after the `:` (C# and TS both
allow this shape; single implementation inheritance keeps lowering trivial). Methods are non-virtual unless
marked `open` (→ C# `virtual`; TS is structurally virtual anyway). `override` is required **both** when
overriding an `open` base method **and** when implementing an interface method (Kotlin's rule — intent is
always explicit; the backend emits C# `override` / explicit interface impl / a plain TS method as
appropriate). Generic bounds use `<T: Bound>`, multiple via `<T: A & B>` (C# `where`; TS intersection /
structural).

### 4.5 Enums vs unions

`enum` is **only** for simple named constants. Data-carrying variants are a `union` (ADT):

```pg
enum Direction { North, East, South, West }
enum HttpCode { Ok = 200, NotFound = 404 }   // explicit values allowed

union Shape {
  Circle(radius: f64)
  Rect(width: f64, height: f64)
  Empty                                       // payload-free case
}
```

| | C# | TypeScript |
|---|---|---|
| `enum` | `enum` | `enum` (or const-union; backend choice) |
| `union` | abstract `record` base + sealed case records, matched by switch | tagged union `{ tag: 'Circle', radius: number } \| …` + `switch (x.tag)` |

`match` over a `union` is **exhaustiveness-checked at compile time** (P3) — a missing case is an error, not
a fall-through.

---

## 5. Statements & control flow

```pg
if x > 0 { … } else if x < 0 { … } else { … }
let m = if a > b { a } else { b }          // `if` is also an expression

while running { … }
do { … } while again

for i in 0..n { … }        // 0..n  exclusive ; 0..=n inclusive
for item in items { … }    // any Iterable<T> (§7)

return value
break ; continue
```

Loops iterate **either a range or an `Iterable<T>`** — one form, `for x in seq`. Ranges (`a..b`, `a..=b`)
are recognized by the backends and emitted as C-style `for` loops where the bound is a plain integer range
(readable output), or as iterator loops otherwise. `if`/`match` are expressions, so they can produce values.

### 5.1 Exceptions (`try`/`catch`/`when`/`finally`, `throw`)

```pg
class ParseError : Error { init(msg: string) { super(msg) } }

fn parseTier(s: string): i32 {
  if s.isEmpty() { throw ParseError("empty tier") }
  return s.toI32()
}

try {
  let t = parseTier(input)
} catch (e: ParseError) when (input.length < 3) {
  log("short bad input")
} catch (e: Error) {
  log("other: " + e.message)
} finally {
  cleanup()
}
```

`Error` is the root exception type (→ C# `System.Exception`, TS `Error`). Typed `catch` lowers to C#
typed catch / TS `instanceof` dispatch with rethrow of unmatched types; `when` → C# exception filter / TS
`if`-guard-then-rethrow; `finally` is identical on both.

### 5.2 Deterministic disposal (`use`)

```pg
use file = openFile(path) {
  file.writeLine(data)
}   // file.dispose() runs at block end, even on throw
```

The bound value must be `Disposable` (`interface Disposable { fn dispose(); }`). Lowers to C# `using`
(or `using` declaration) and TS `try { … } finally { file[Symbol.dispose]() }`. Disposal is deterministic
on both — which is exactly why finalizers/GC hooks are refused (§10): `use` is the whole disposal story.

---

## 6. Operators, properties, indexers, extensions

### 6.1 Operator overloading (named methods → symbols)

TypeScript has **no** operator overloading, so a custom `a + b` must lower to a method call there. Polyglot
therefore defines operators as **fixed-named methods**; the C# backend emits real `operator` members and
the TS backend rewrites the symbol to the method call.

| Operator | Method name | | Operator | Method name |
|---|---|---|---|---|
| `+` | `plus` | | `==` | `eq` |
| `-` (binary) | `minus` | | `<` `<=` | `lt` `le` |
| `*` | `times` | | `>` `>=` | `gt` `ge` |
| `/` | `div` | | `-` (unary) | `neg` |
| `%` | `rem` | | `&` `\|` `^` | `band` `bor` `bxor` |

```pg
record Money(cents: i64) {
  operator fn plus(o: Money): Money => Money(this.cents + o.cents)
  operator fn lt(o: Money): bool => this.cents < o.cents
}
// a + b  -> C#: a + b   |  TS: a.plus(b)
// a < b  -> C#: a < b   |  TS: a.lt(b)
```

`eq`/`lt` also feed `match` literal/comparison patterns and the generated structural equality.

### 6.2 Properties and indexers

```pg
class Circle {
  var radius: f64
  let area: f64 => 3.141592653589793 * this.radius * this.radius   // read-only computed
  var diameter: f64 {
    get => this.radius * 2.0
    set(v) { this.radius = v / 2.0 }
  }
}

class Grid<T> {
  operator fn get(x: i32, y: i32): T { … }
  operator fn set(x: i32, y: i32, value: T) { … }
}
// grid[x, y]  and  grid[x, y] = v
```

Properties → C# properties / TS `get`/`set` accessors. Indexers → C# `this[…]` / TS `.get(…)`/`.set(…)`
method calls (TS index signatures can't take multiple keys, so the call form is used uniformly).

### 6.3 Extension functions

```pg
extension fn string.shout(): string => this.toUpper() + "!"
extension fn List<T>.second(): T? => if this.count >= 2 { this[1] } else { null }
// "hi".shout()  ->  C#: static extension call  |  TS: shout("hi")
```

---

## 7. Iterators (`yield`) and sequences

```pg
fn countUp(n: i32): Iterable<i32> {
  var i = 0
  while i < n { yield i; i += 1 }
}
for v in countUp(3) { log(v) }     // 0 1 2
```

A function whose body uses `yield` is an iterator; its return type is `Iterable<T>`. Lowers to C#
`IEnumerable<T>` + `yield return` and TS `function*` + `yield`. `for x in seq` consumes any `Iterable<T>`
(C# `foreach` / TS `for…of`).

---

## 8. Strings & characters

Both targets are **UTF-16** — `string` is a sequence of UTF-16 code units on both, so the mapping is near
1:1. `char` is a single UTF-16 code unit (a `number` in TS, `char` in C#). Code points outside the BMP are
surrogate pairs; the std string API exposes code-point iteration for correct handling, and the spec warns
(as C# and JS both do) that indexing yields code *units*, not code points. String interpolation
`"hi ${name}"` lowers to C# `$"hi {name}"` and TS template literals — both first-class on each target.
String `==` is value equality on both targets (C# `string ==`, TS `===`).

---

## 9. Faithful-by-default & the published relaxation list (PRD §3.C)

These are the points where C# and JS genuinely differ. Default behaviour is the faithful one; each
relaxation is **opt-in and named**, never silent:

| Concern | Default (faithful) | Relaxation (opt-in / documented) |
|---|---|---|
| `i32`/`u32`/sub-32 overflow | masked at op boundaries in TS (`\| 0`, `>>> 0`, `Math.imul`) so JS wraps like .NET | intermediate values below 2⁵³ that .NET would wrap are *not* re-masked unless typed `i32` — made explicit, not silent |
| `i64`/`u64` | TS `bigint` (correct, slower) | a two-word `Long` class may be offered later if `bigint` is too slow |
| `f32` vs `f64` | `f32` rides a JS `number` (double) | per-op `Math.fround` strict-float mode is opt-in (the Scala.js strict-floats tax) |
| nullability | `null`/`undefined` normalized to one "absent" | — |
| equality / hashing | `record` gets structural `==`/hash; `class` is identity; identity hash via side `WeakMap` in TS | — |

**Determinism honesty (PRD §3.D):** only `+ − × ÷ √` are reproducible across .NET and JS (at matched
width). Transcendentals (`sin`/`cos`/`exp`/`pow`), FMA, and reassociation diverge. Code that needs
identical cross-target results must use the std **fixed-point / soft-float** type (a P7 std module), *not*
`f32`/`f64`. The FruitCake sketch uses only `+ − × ÷ √`, so its eventual conformance test gates on
tolerance + behaviour, never bit-equality.

---

## 10. Refused — not in the grammar (PRD §3.B)

These have **no production** in [`grammar.ebnf`](grammar.ebnf). You cannot write them; if you reach for the
underlying platform API anyway, the type/name simply does not resolve, and P6 adds a *targeted* refusal
diagnostic (e.g. naming `Thread` or `decimal`) so the message is "Polyglot refuses X because …", never a
miscompile.

- **Threads / concurrency primitives** — `lock`, `Interlocked`, `Parallel.*`, thread spawning. Single-
  threaded `async`/`await` only (§4 reserves `async`; full semantics specced when first exercised).
- **Runtime reflection** — `Activator`, open-world `Type.GetType(string)`, attribute introspection at run
  time. (Defeats tree-shaking; #1 bloat source.)
- **Finalizers / GC hooks** — no `~T()`, no `GC.*`. Deterministic disposal is `use` (§5.2) — that's all.
- **`decimal`** — no `decimal` type or literal (a big-decimal std type may be opted in later).
- **`unsafe` / pointers** — no `unsafe`, `*T`, `&`-address-of, `stackalloc`, raw `Span`.
- **`dynamic` / runtime code-gen** — no `dynamic`, `Reflection.Emit`, `Expression.Compile`.
- **Bit-exact cross-target floats** — not promised; see §9 / PRD §3.D.

---

## 11. Module structure & imports (provisional)

A `.pg` file is one compilation unit. Top-level declarations are module-public unless marked `private`.
Imports name other modules:

```pg
import std.math.{ sqrt, PI }
import std.collections.List
import app.physics as phys
```

Full package/resolution rules are deferred to P2/P3 (this is the design milestone); v0.1 fixes only the
*syntax* of imports and visibility. `expect`/`actual` (target-gated capabilities) and `extern` (the FFI
hatch) are **reserved keywords** with sketched syntax in [the P7 plan](../prd/PLAN.md) — out of the v0.1
core grammar on purpose.

Platform/SDK/environment APIs (`window`, `System.Windows.Forms`, …) are **not** in the core language at
all: the core is a pure translator + a declarative emit engine, and all such APIs — plus the **target
backends themselves** — arrive via **declarative plugins** named in a workspace config (`pgconfig.json`) that
also declares the target *environments* (desktop/web/mobile/…). Downloaded plugins are declarative data
(safe, versioned); local plugins may be full-power. A `.pg` module that imports a plugin's public API is
constrained to that plugin's targets/environments (off-target use is a compile error). See the design note
**[`../design/plugins-and-targets.md`](../design/plugins-and-targets.md)**.

---

## 12. Worked mapping example (end-to-end intent)

```pg
union Shape { Circle(r: f64), Rect(w: f64, h: f64) }

fn area(s: Shape): f64 => match s {
  Circle(r)  => 3.141592653589793 * r * r,
  Rect(w, h) => w * h,
}
```

**Intended C#:**
```csharp
abstract record Shape;
sealed record Circle(double R) : Shape;
sealed record Rect(double W, double H) : Shape;

static double Area(Shape s) => s switch {
    Circle c   => 3.141592653589793 * c.R * c.R,
    Rect r     => r.W * r.H,
};
```

**Intended TypeScript:**
```ts
type Shape = { tag: "Circle"; r: number } | { tag: "Rect"; w: number; h: number };

function area(s: Shape): number {
  switch (s.tag) {
    case "Circle": return 3.141592653589793 * s.r * s.r;
    case "Rect":   return s.w * s.h;
  }
}
```

Both are idiomatic and readable in their target — the v0.1 acceptance bar (PRD §2). The full sample set in
[`samples/`](samples/) exercises every §3.A feature this way; [`samples/fruitcake_sketch.pg`](samples/fruitcake_sketch.pg)
pressure-tests the surface against a real program (the MintPlayer.AI FruitCake physics).

---

## 13. Sample index

| File | §3.A features exercised |
|---|---|
| `01_functions.pg` | functions, overloading, default params, recursion, lambdas/closures, expression bodies |
| `02_records_operators.pg` | records, operator overloading, computed properties, structural equality, `with` |
| `03_enums_unions_match.pg` | enums, unions (ADTs), pattern matching + exhaustiveness, guards |
| `04_generics.pg` | generic types & functions, constraints, interfaces, indexers |
| `05_iterators.pg` | `yield` iterators, `for…in`, ranges, lazy sequences |
| `06_exceptions.pg` | custom errors, `throw`, typed `catch`, `when` guard, `finally` |
| `07_using_disposal.pg` | `Disposable`, `use` deterministic disposal |
| `08_extensions.pg` | extension functions on `string` and generic types |
| `09_strings.pg` | interpolation, `char`, UTF-16 / surrogate handling, value equality |
| `fruitcake_sketch.pg` | **north-star surface test:** mutable classes, records, generics, nullability, tuples, `match`, lambdas, ranges, std math — modeled on the real FruitCake solver |
