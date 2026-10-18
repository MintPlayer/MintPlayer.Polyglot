# MintPlayer.Polyglot — Language Specification (v0.1)

> **Status:** Draft v0.1 · 2026-06-28 · P1 deliverable (design-on-paper, no compiler yet).
> Companion to the formal grammar in [`grammar.ebnf`](grammar.ebnf) and the sample programs in
> [`samples/`](samples/). Coming from another language? Side-by-side construct maps:
> [`for-csharp-devs.md`](for-csharp-devs.md) · [`for-typescript-devs.md`](for-typescript-devs.md). The PRD's §3 support/refuse contract ([`../prd/POLYGLOT_PRD.md`](../prd/POLYGLOT_PRD.md))
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
| Identifier | `[A-Za-z_][A-Za-z0-9_]*` | may not contain `__` (two underscores) anywhere: that spelling belongs to compiler-generated names — lowering and emitter temporaries (`__c0`, `__opt1`), the std primitives (`__pg…`) and overload signature names (`parse__string`) — so a user name can never collide with one (*identifier '__c0' contains '__', which is reserved for compiler-generated names*) |
| Int literal | `42`, `0xFF`, `0b1010`, `1_000`, `42i64`, `255u8` | default type `i32`; `_` separators allowed |
| Float literal | `3.14`, `1e-9`, `2.0f`, `64f` | default type `f64`; suffix `f`/`f32` = `f32`, `d`/`f64` = `f64` |
| Bool | `true`, `false` | |
| Char | `'a'`, `'\n'`, `'\u{E9}'` | exactly one UTF-16 code unit (§8): empty, multi-character and astral char literals are errors |
| String | `"…"`, `"hi ${name}!"` | interpolation with `${ expr }`; UTF-16 (§8) |
| Escapes | `\n` `\t` `\r` `\0` `\\` `\"` `\'` `\$` `\u{1F600}` | the complete set, shared by strings and chars. `\$` is a literal `$` (`"\${x}"` is the text `${x}`). `\u{…}` takes 1–6 hex digits, ≤ `10FFFF`, never a surrogate. Any other escape — `\q`, `\uXXXX`, `\xHH` — is an error at the backslash. Literals must be well-formed UTF-8. |
| Null | `null` | only assignable to a nullable type `T?` (§4.3) |

Statements are **newline-terminated**; `;` is an **optional separator**, needed only to place several
statements on one line (`a = 1; b = 2`). The sample programs omit it. Blocks `{ }` need no trailing `;`.
A call's `(` and a subscript's `[` must start on the **same line** as the expression they apply to: a line that
begins with `(` or `[` starts a new statement, never a continuation of the previous one (so
`((x: i32) => print(x))(8)` on its own line is an immediately-invoked lambda, not a call on the line above).
An expression continues onto the next line after a token that cannot end it (a binary operator, `,`, an open
`(`/`[`), and a line that begins with `.` or `?.` continues a member chain.
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
default is `i32`; float-literal default is `f64`.

**Numeric conversions use a cast `(T)value`** — there are no `toI64()`/`toU32()`/… conversion methods
(they would be a combinatorial swamp). **Lossless widenings are implicit**: a narrower value flowing into
a wider slot (assignment, call argument, return, mixed-width arithmetic) is converted automatically —
`i8→i16→i32→i64`, `u8→u16→u32→u64`, `i32→f64`, `f32→f64`. **Everything else needs an explicit cast**:
narrowing (`(i32)big`), lossy widening (`(f64)someI64` — loses precision past 2⁵³), and sign changes
(`(u32)signed`). A cast never fails silently and never miscompiles; it truncates toward zero (float→int)
and wraps (narrowing) exactly as .NET does, mirrored on the JS side (`BigInt`/`Number`/`Math.trunc`/masking).
Converting `string`↔number is **not** a cast — text parsing can fail (and C# can't cast `string`→`int`),
so it is a fallible **static method on the type**: `i32.parse(s)` / `f64.parse(s)` (throwing an `Error`) and
`i32.tryParse(s): i32?` / `f64.tryParse(s): f64?` (absent instead), on every integer and float scalar. A std
API (§7/P7), separate from casts. Both accept **one strict grammar on every target**, checked in std.core's
own `.pg` before any native parser runs:
- an integer (`i8`…`u64`): `[+-]?[0-9]+` whose value is in the type's range (`"007"`, `"+7"` and `"-0"` are
  fine; `u8.parse("-0")` is `0`);
- a float (`f32`, `f64`): `[+-]?(digits[.digits*] | .digits)([eE][+-]?digits)?`; a value too large reads as
  an infinity.

Nothing else is a number: no surrounding whitespace, no `_` separators, no `0x`/octal prefix, no `NaN` /
`Infinity`, no empty string, no non-ASCII digits. `"12a"`, `"1e3"` and `"2147483648"` all make
`i32.parse` throw `Error("i32.parse: '12a' is not a valid i32")` and `i32.tryParse` return `null`, identically
on C#, TS, Python and PHP (std.strings' `s.toI32()` is `i32.parse(s)`). Parsing is culture-free (§8).

Converting `char`→number is **not** a cast either — a char is a native char only on C# but a
one-character string on TS/Python/PHP, so `(i32)c` cannot be honored uniformly (it printed 65 on C#, 0 on
TS/PHP, and dropped the cast on Python before it was refused — issue #34). The portable ordinal accessor
is **`s.codePointAt(i): i32`** in `std.strings` — the code point starting at UTF-16 unit `i` (a pair's high
half yields the whole code point, a lone surrogate its own unit), identical on all four targets (§8).

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

The type annotation is **optional when the initializer determines the type** (`let x = 41`, `let e = [1,2,3]`,
`var d = List<i32>()`) and **required when it does not**. An *un-inferable* initializer — an empty list
literal `[]`, a bare `null`, or a lambda with un-annotated parameters — conveys no type for the compiler to
reconstruct, so it must be annotated; otherwise it is a compile error (rather than a silent per-target
guess). This is the same discipline Swift and Go enforce, and it guarantees every target is handed a
concrete type:

```pg
var d: List<i32> = []      // required: `[]` alone has no element type
var x: Node? = null        // required: bare `null` has no underlying type
let e = [1, 2, 3]          // fine: element type inferred from the members
```

`let` and `const` may also appear at **module scope** (top-level). A top-level `const` is a compile-time
scalar; a top-level `let` is an immutable binding initialized at module-init time and may hold an aggregate
(→ C# `static readonly` / TS module-level `const`). A **list literal** `[a, b, c]` builds a `List<T>`
(→ C# collection expression `[…]` / TS array `[…]`):

```pg
let catalog: List<i32> = [ 16, 22, 30, 40 ]   // module-level immutable list
```

**Arrays vs lists.** There are two sequence types, split by whether the size can change:

| Source | Semantics | C# | TypeScript | Python | PHP |
|---|---|---|---|---|---|
| `T[]` (array) | fixed-size: element get/set, `.count`; **no** add/remove | `T[]` | `T[]` | `list` | `array` |
| `List<T>` (list) | growable: `.add` / `.removeAt` / … | `List<T>` | `T[]` | `list` | `array` |

Both spell a **JavaScript array** on the TS target (a JS array already has push/pop/splice), so a `T[]` and a
`List<T>` are indistinguishable at runtime there; on C# the array stays a distinct `T[]`. `T[]` is surface
sugar for the core `Array<T>` type (no import); the fixed-size contract is enforced by the type checker
(`a.add(…)` on a `T[]` is a compile error). A list literal takes the type of its declared slot, so
`var a: i32[] = [1, 2, 3]` is an array while `var xs: List<i32> = [1, 2, 3]` is a list. A collection of a
**union** element composes safely on every target — e.g. `Node?[]` is C# `Node?[]` and TS `(Node | null)[]`
(the union is parenthesized so the postfix `[]` binds correctly).

**Maps.** `std.collections` also declares an insertion-ordered dictionary (P40 B4):

```pg
extern class Map<K: IMapKey, V> {
  constructor()
  let count: i32
  fn get(key: K): V?       // null when absent
  fn set(key: K, value: V) // re-setting a key keeps its position; remove + set moves it to the end
  fn has(key: K): bool
  fn remove(key: K): bool  // true when the key was present
  fn keys(): List<K>       // a SNAPSHOT, in insertion order
  fn values(): List<V>     // a SNAPSHOT, in insertion order
}
```

| | C# | TypeScript | Python | PHP |
|---|---|---|---|---|
| `Map<K, V>` | `OrderedDictionary<K, V>` (**.NET 9+**) | `Map<K, V>` | `dict` | `\ArrayObject` (string keys stored as `"s".$k`) |

- **Keys** are `string` or an integer scalar (`i8`..`u64`) — the `IMapKey` marker bound, which C# spells
  `where K : notnull` and the other targets erase. Refused, each with its reason: floats (NaN, `-0.0`),
  `bool` (Python's `True == 1`, PHP's 0/1 coercion), `char` (a string on three targets, an integral type on
  C#), records (no target hashes them structurally), classes/interfaces (identity is not a PHP array key),
  nullable keys, and anything else (enums, collections).
- **`V` must not be nullable**: `get` returns `V?`, so a stored null would read as an absent key. Test presence
  with `has`. (The rule is general: an extern class with a member typed `T?` refuses a nullable `T`.)
- **A `Map` is not directly `for`-iterable** — the natives yield keys, pairs or values, and mutation while
  iterating differs on every target. Iterate `m.keys()` or `m.values()`; they are copies, so mutating the map
  inside the loop is safe and not observed by it.
- **Costs:** `remove` is O(n) on C# (`OrderedDictionary` shifts its entry array) and O(1) elsewhere;
  `keys()`/`values()` copy, O(n). The PHP target keeps string keys prefixed so `"10"` never becomes the
  integer `10` (a bare PHP array coerces numeric strings), and decodes them in `keys()`.

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
  let id: string           // set once in the constructor
  var balance: f64

  constructor(id: string, balance: f64) {
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

A record's body holds **methods, operators and computed properties only** (and `static fn`s). Its state is
its positional fields — they alone take part in equality, hashing and `with` on every target — so a body
field, static or not, is refused (*record 'P' declares a body field 'twice'*): make it a positional field
(with a default) or a computed property; a constant belongs in a module-level `let`/`const`. A positional
field's default is filled in at every construction that omits it (`record P(x: i32, y: i32 = 5)`: `P(1)` is
`P(1, 5)` on every target — P40 review r5); defaults follow the same constant rule as parameter defaults (§5).

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

**`x!` is a checked assertion on every target** (P40 A4). On a nullable operand it yields the value, or
throws an `Error` whose message names the source site — `null-assert failed (m.pg:L:C)`, identical on
all four targets (C# `NullReferenceException`, TS/Python `TypeError`, PHP `UnexpectedValueException`; a
`catch (e: Error)` catches all of them). The operand is evaluated once and the form is valid as an
assignment target (`box.next!.v += 1`). On a non-nullable operand `!` is a no-op. On an optional generic
(`T?` where `T` is a type parameter, i.e. `Option<T>`), `opt!` unwraps `Some` and throws the same error
on `None`.

**Nullable operands.** Every target treats null differently in arithmetic, comparison, concatenation and
stringification, so a maybe-null value must be handled before it is used:

- A nullable operand to any operator except `==`, `!=` and `??` is an error (*'+' operand of type
  'string?' may be null; supply a fallback with '??' or assert with '!'*). That includes unary operators,
  `&&`/`||`, a `bool?` condition (`if`, `while`, `when`) and a compound assignment (`n += 1` on `i32?`).
- A nullable value in an interpolation hole (`"${n}"`) or passed to `print` is an error.
- `string + T` with a type parameter `T` is an error — interpolate instead (like `string + i32`).
- `.member` / `.method()` on a nullable receiver is an error — use `?.`, narrow it, or assert with `!`.
- An if-expression with a `null` branch is nullable: `if c { 1 } else { null }` is `i32?`.
- A maybe-null value flowing into a non-null slot — a return, an initializer, an argument, an assignment,
  a field — is an error (*cannot convert 'i32?' to 'i32' … the value may be null*), including `null` itself
  and `a ?? b` whose fallback `b` is nullable (P40 review r4). A narrowed local converts. Every slot is held
  to the same rule (P40 review r5): each **arm** of a `match`/`if` value converts on its own (a match's type
  joins its arms — nullable if any arm is, numeric arms widen), a `yield`ed value converts into the
  iterator's element type, a record `with` override into its field, an index into `i32` (a user indexer:
  its `get` parameters), and a tuple literal element by element.
- **Type arguments must match exactly** (generic types are invariant): a `List<i32?>` is not a `List<i32>`
  (C# CS0029), nor a `Box<string?>` a `Box<string>`. An `Iterable<T>` target reads only, so it judges just the
  element's nullability (`List<i32?>` into `Iterable<i32>` is refused). A tuple value's elements follow the
  slot rule.
- A **type-parameter slot** `T` may be instantiated with a non-null type, so `null`, a maybe-null value or a
  `T?` (an `Option<T>` there, §4.4) cannot go into it (`x: T = null`, `fn f<T>(): T => null`) — declare it `T?`.
- A **lambda** flowing into a function type must take exactly its parameters: the same arity, and each
  annotated parameter exactly the slot's type (`(x: i32) => …` where `(i32?) => i32` is expected is refused —
  C# CS1661). An unannotated parameter takes the slot's type.
- `recv?.m(args)` on a maybe-null receiver — a method, a std member (`s?.len()`, `m.get(a)?.get(b)`), an
  extension, a function-typed field (`h?.f(5)`) — calls only when `recv` is present and yields `T?` (so
  `let w: i32 = r?.val()` is refused); the receiver is evaluated once. As a statement (`r?.run()`) it is an
  `if`. On a receiver that cannot be null `?.` is a plain call. A `?.` read is `null` when absent on every
  target (TS's `undefined` is normalized), so `b?.n == null` agrees. Chaining is explicit: `a?.b()?.c` (a
  `.c` on the nullable `a?.b()` is the nullable-receiver error, unlike C#'s whole-chain short-circuit).
- `t ??= v` assigns `v` only when `t` is null; `t` must be able to be null (a non-null or narrowed target is
  refused, like `??`), `v` converts into `t`'s type, and a target without a native `??=` (Python) evaluates
  it as `if t == null { t = v }` with `t`'s receiver evaluated once.
- A nullable **function** type `(() => unit)?` is spelled nullable on the typed targets (C# `Action?`, TS
  `(() => void) | null`); a narrowed function-valued local or parameter is callable (`if cb != null { cb() }`).

**Null flow-narrowing.** A nullable **local or parameter** is non-null where a test proves it:

```pg
fn len(s: string?, t: string?): i32 {
  if s == null { return 0 }           // early exit: `s` is string for the rest of the block
  if t != null && t.len() > 3 { … }   // `&&`: the right operand sees `t` as string
  return if t != null { s.len() + t.len() } else { s.len() }
}
```

Narrowing applies after `if x != null` (the then-branch), `if x == null` (the else-branch), an `if`
whose branch always exits (`return`/`throw`/`break`/`continue` — the rest of the enclosing block),
`x != null && …`, `x == null || …`, and `while x != null` (the loop body). It ends at the end of that
region and at any assignment to the variable; a loop that assigns the variable is never narrowed inside
it, a lambda body never sees an outer narrowing, and a `var` that some lambda assigns is never narrowed.
A test **inside** a lambda narrows a captured parameter, `let` or (lambda-unassigned) `var` for the rest of
that lambda body, exactly as in a function body (P40 review r5) — a lambda runs synchronously, so nothing
outside it can reassign the variable between the test and the read.
**Fields and other expressions are not narrowed** — any call between the test and the read may reassign a
field — so `if this.name != null { shout(this.name) }` is refused with that rule named: read it into a local
(`let v = this.name`, then test `v`), or use `!`. `!` on a narrowed variable is allowed and is a no-op.

### 4.4 Interfaces, inheritance, generics

```pg
interface Comparable<T> { fn compareTo(other: T): i32; }

open class Shape { open fn area(): f64 => 0.0 }
class Disk : Shape { 
  let r: f64
  constructor(r: f64) { this.r = r }
  override fn area(): f64 => 3.141592653589793 * this.r * this.r
}

fn maxOf<T: Comparable<T>>(a: T, b: T): T => if a.compareTo(b) >= 0 { a } else { b }
```

A class has **at most one base class** and any number of interfaces, all after the `:` (C# and TS both
allow this shape; single implementation inheritance keeps lowering trivial). Methods are non-virtual unless
marked `open` (→ C# `virtual`; TS is structurally virtual anyway). **`override` follows C# conventions**:
it marks only an override of an `open`/`abstract` **base-class** member — implementing an interface method
takes **no** `override`, and writing one there is an error (as is `override` with no base-class member).

**Construction runs the whole base chain** (P40 review r4) on every target: a subclass constructor calls
the base constructor whether or not it writes `super(…)` (an omitted call is `super()`), and a subclass with
no constructor still runs it — including a base whose only constructor work is a field initializer. When the
base constructor takes arguments, the subclass must declare a constructor that writes `super(…)`, and
`super(…)` must be its **first statement** (P40 review r5): nothing may run before the base constructor
(C# `: base(…)` cannot see a local, and the other targets would silently run a statement written first after
it) — compute the arguments inline or in a `static fn`. An inherited field is therefore assigned before the
subclass constructor's first statement and may be read there (`super(); this.y = this.x + 10`). A subclass's
own field initializers run **after** the base constructor on every target (C# natively runs them before it, so
the C# plugin assigns a subclass's non-literal initializers in its constructor, right after the base call).

A constructor of a class that **can be subclassed** — `open` or `abstract`, or the base of some class in the
program — may **not** call an overridable (`open`, `abstract` or `override`) method on the object under
construction: a subclass override would run before the subclass's own fields are initialized (C# reads
defaults, TS `undefined`, Python raises `AttributeError`). Since P40 review r6 this is a **local rule** on what
the object may do while it is under construction, rather than a search for the paths that reach an override.
In such a constructor, `this` — or a value derived from it: an alias (`let me = this`), a lambda that captures
it, a local holding one, an own field that some body assigns one — may only:
- read or assign its **own fields**, store a derived lambda into its own field (a callback set in a constructor),
  or bind one to a local;
- call its own **non-open** methods, computed properties (getters and setters), indexers and extensions — whose
  bodies follow the same rule, transitively;
- run a derived lambda whose body is known: one invoked in place, or called through the local or parameter
  bound to it (its body follows the rule);
- be passed as an argument to a callee whose body is **statically known** — a free function, a static method,
  another class's constructor, another object's **non-open** method, an extension, a lambda — where the
  parameter that receives it obeys this same rule (so nothing holds the object once the call returns).

Everything else is refused, naming the use: calling an `open`/`abstract`/`override` member on it (*… calls the
overridable method 'describe' on 'this' (through 'show')*); passing it where the body that runs is chosen at run
time or is not a `.pg` body (an interface or open method, a std/extern member such as `List.add`, a function
value — *… passes 'this' as an argument to 'accept', whose body is not known here*); putting it into a
collection, tuple, record or another object, storing it in another object or a module value (also inside a
callee: *… stores 'this' (as 'p') in another object (through 'Probe')*), returning it, using it as an operand;
and calling an own function-typed field that may hold a lambda capturing it (*… calls the field 'cb' (it may
hold a lambda that captures 'this')*). The rule over-approximates on purpose: a callee that only **stores** the
lambda it is given (an event subscription, `bus.subscribe(() => this.refresh())`) is refused too, because the
lambda then lives in an object the constructor could call into next (`bus.fire()`), and proving it does not
would need whole-program aliasing. Subscribe after construction instead. A class nothing can derive from is
not checked at all — it may call its own `override` or an inherited open method and pass `this` anywhere:
`this` is exactly that class. Call it after construction, or make it non-open.

**Abstract members** (P40 review r5): an `abstract class` may declare `abstract fn` signatures (no body); it
is never constructed, and a concrete subclass must override every abstract method it inherits. Each target
declares real abstract members — C#, TS and PHP `abstract`, Python `@abc.abstractmethod` on an `abc.ABC`
class. `abstract` on anything but a method, an abstract method with a body or in a non-abstract class, and
constructing an abstract class are refused. A **static** member is never `open`, `override` or `abstract`
(statics are not dispatched virtually; C# CS0112).

An **interface body holds bodiless, non-static method signatures only** — no fields, properties,
initializers, operators, or default method bodies (express field-like needs as a getter method). The
checker **enforces conformance**: a class or record declaring `: I` must implement every method `I` (and
its base interfaces) declares, with matching signatures — generic interfaces substitute their type args
(`record Version : Comparable<Version>` must implement `compareTo(Version): i32`) — and a value of a
class type converts to an interface type only when the class declares it implements it (nominal, not
structural). Per-target lowering: C#/TS/PHP `interface` + implements clause; Python emits
`class I(abc.ABC)` with `@abc.abstractmethod` stubs (nominal ABC, not a structural `Protocol`).

Generic bounds use `<T: Bound>`, multiple via `<T: A & B>` (C# `where`; TS intersection /
structural). One built-in bound is **`INumber`** — a core marker constraint (like .NET's
`System.Numerics.INumber<T>`) satisfied by the numeric scalars (`i8`..`u64`, `f32`, `f64`); `std.math`'s
`min`/`max`/`abs`/`round` use it, and a non-numeric type argument is rejected at Polyglot compile time. It is
compile-time-only — erased from the emitted C#/TS, which already type-check numeric operations.
Bounds are enforced on **generic types as well as functions** — on every spelled type
(`let b: NumBox<string>` is refused for `class NumBox<T: INumber>`), on explicit construction type
arguments, and on inferred ones (`NumBox("x")`). An interface/class bound needs that type or a subtype
(nominal, by name); a type parameter forwarded from an enclosing generic is not judged.

**Members of a generic type are typed through the receiver** (P40 slice 7): `Cell<string>.first(): V`
is a `string`, `Box<i32>.value` an `i32`, `Box<i32>.set(v: T)` takes an `i32`, and a member inherited
through `class D<U> : B<List<U>>` sees `B`'s parameter as `List<…>`. A construction without type arguments
infers them from the constructor arguments (`Cell(3)` is `Cell<i32>`, and every target spells it). A
user class's `T?` member reads as `Option<i32>` (the representation its body uses, exactly like a generic
function's `T?` result); an **`extern class`**'s `V?` member is a *native* nullable once `V` is concrete
(`int?` on C#), and is refused while `V` is still a type parameter (a binding's native null has no
faithful form there, §3.C).

**The `T?`-representation boundary** (P40). Those are two distinct types with one meaning: `Option<X>` (a
user generic's `T?` at a concrete use site) and the native `X?` (a concrete type's nullable). The checker
converts between them wherever a value crosses from one to the other, and the null is always the `None`:

| Position | `Option<X>` → native `X?` | native `X?` → `Option<X>` |
|---|---|---|
| `let`/`var` with an annotation, assignment (local, field, element), list-literal element, argument (incl. a generic method's or extension's `U?`), return, a lambda's result (expression body or each `return` of a block body, against the function type it meets), field / record init | `match o { Some(v) => v, None => null }` | `match n { null => None, v => Some(v) }` — never `Some(null)` |
| `==` / `!=` against a native operand (`null`, `string?`, `string`) | compares as `X?`: `slot.peek() == null` is an absence test | — |
| `!`, `?? d` | unwraps (`None` throws / takes `d`) | — |
| `if o != null` on an `Option` local | the narrowed read is the unwrapped `X` | — |

A generic function's `T?` parameter given a native `string?` (or a bare `string`) binds `T = string`, not
`string?`, and the argument converts at the call. Refused, with a diagnostic: an `Option<X>` into a
non-nullable `X` slot (it may be absent; unwrap with `??`, `!` or a match), and `Option<X?>` into `X?`
(`Some(null)` and `None` would both become null). Untouched by the boundary: a `match` on the `Option` itself
and `Option` to `Option`. A `null` match pattern is an identity test on every target (PHP `=== null`, Python
`is None`), so a present `0` or `""` is never read as absent.

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

Union values compare **structurally** with `==`/`!=`, as C# compares the case records: the same case with equal
payloads, a record payload by its own equality, a nested union recursively, and anything else — a class
instance, a list — by reference (P40 review r5: TS and PHP compare through a prelude helper; TS compared with
`JSON.stringify`, so a class payload compared by content and an `i64` payload threw, and PHP compared the tagged
arrays with `===`, so a record payload compared by identity).

### 4.6 `is` / `as` (P37 B)

Runtime type tests and checked conversions over **class/record hierarchies** (unions/cases stay `match`'s
job — union values are tagged data and cases are not types to narrow to):

```pg
if x is Circle c && c.r > 0 { print(c.r) }   // `is T name` binds a narrowed local — if-conditions only,
                                             // scoped to the guarded branch (and the rest of the `&&` chain)
let ok = x is Circle                         // bare bool form, any expression position
let d = a as Dog                             // checked conversion -> Dog? — null on failure, NEVER throws
print(d?.nick ?? "not a dog")
```

Emission: C# `is`/declaration-pattern + `as`; TS/PHP `instanceof` guards (never a bare TS `as` assertion —
that has no runtime check); Python `isinstance`. The `if`-binding form lowers to a hoisted
`let name = x as T` + `name != null`, so the emitted binding **outlives the branch** — redeclaring the
name later in the same block is refused. Refused: tests on unions/cases, enums, scalars, extern/std types,
type parameters (generics are emitted generically — no runtime test can exist), generic instantiations
(type args erase on TS/Python), and provably-unrelated types. Interface tests gate per-target
(`interfaces:runtimeIdentity`, §10.1 #38).

### 4.7 Attributes & portable metadata (P37 D — the three-tier taxonomy)

One usage syntax — `[Name(args)]` before a class/record/function/method — two declaration forms, and the
declaration picks the tier:

```pg
// Tier 2 — portable inert metadata: a pure DATA SHAPE (no body, no constructor, const-envelope params).
attribute Range(min: i32, max: i32)

[Range(1, 8)]
class Box {
  [Range(0, 100)]
  var fill: i32
  constructor() { this.fill = 0 }
}

fn main() {
  print(Meta.has<Box, Range>())                 // true      — a bool literal in the output
  print(Meta.get<Box, Range>()?.max ?? -1)      // 8         — `new Range(1, 8)` inlined at the call site
  print(Meta.member<Box, Range>("fill")?.max ?? -1)         // 100
  print(Meta.param<Box, Range>("scale", "k")?.max ?? -1)    // parameter metadata, same discipline
}

// Tier 1 — pass-through: the binding IS the native annotation line, per target, emitted verbatim
// above the declaration for that target's OWN framework; `import` is a verbatim header line.
extern attribute JsonProp(name: string) {
  actual(csharp)     extern("[Newtonsoft.Json.JsonProperty($0)]") import "using Newtonsoft.Json;"
  actual(typescript) extern("@JsonProperty($name)")               import "import { JsonProperty } from 'class-transformer';"
  actual(python)     refuse
}
```

- **Tier 2 is fully inside §9 faithfulness**: every `Meta` query resolves **at transpile time** and lowers
  to inline constants — no metadata file, no table, no reflection, identical on all four targets, and an
  attached-but-unqueried attribute emits **zero** runtime output. No capability is involved (the compiler
  both writes and reads the data), so it works on every target unconditionally. Queries are exact-type,
  definition-site lookups yielding a fresh value per evaluation — no inherited-attribute walk. Attribute
  types cannot be constructed directly.
- **Tier 1 is outside the faithfulness guarantee** (H2): Polyglot guarantees the spelling + attachment
  land per the binding, never identical cross-target runtime behavior (C#/PHP attributes are inert
  metadata; TS/Python decorators execute at definition time — bind only semantically-passive
  annotations). Attachment points gate per target (`attributes:target.{type,method,function}` — TS has
  no function decorators); a used attribute with **no arm** (or an explicit `refuse` arm) for a
  configured target refuses loudly (D12) — never a silent drop.
- Attachment points: classes/records, methods, fields/properties, and parameters (both tiers), plus free
  functions (Tier 1 only — `Meta` takes a type, so function metadata has no query surface). Tier 1
  points gate per target: TS has no function or parameter decorators; Python has no field or parameter
  annotations. Values: bool/int/float/string literals, enum members, and non-empty arrays of those —
  each spelled from plugin-spec data (`trueLit`/`falseLit`, `enumMemberOp`, `constArrayOpen/Close`).
  Recorded deferrals: enum-case and `return:` targets, `AllowMultiple`, variable argument values.

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

**Ranges (P40 A2).**
- **Precedence is Rust's:** `..`/`..=` is the *loosest* binary form and does not chain, so `0..n - 1`,
  `-2..2`, `i + 1..n`, `0..=n * 2` and `0..a ?? 5` all mean what they read as (`a..b..c` is an error).
- **A range is only valid as a `for` iterable** — `let r = 0..5`, `(0..n) - 1` or a range argument is an
  error (*a range ('a..b') is only valid as a for-loop iterable*).
- **Bounds must be integers** (`range bounds must be integers (got f64)`). The two bound types widen to one
  (the narrower bound is widened, as for a binary operator), which is the loop variable's type:
  `for i in 0..n64` gives an `i64` `i`. A pair neither side of which widens (`i64` with `u64`) is an error.
- **Bounds are evaluated once**, start then end, before the first iteration — as in Rust, Kotlin, Swift and
  Python. A body that changes what the end expression would read (`n += 1`, `xs.add(…)`) does not change
  the trip count. Output stays readable: an end that cannot change (a literal, a module value, an immutable
  local or parameter) is re-read in the condition (`i < n`); any other end is hoisted into a temporary
  next to the loop variable (C# `for (int i = 0, __end0 = xs.Count; i < __end0; i++)`, TS `let`, PHP comma
  init; Python's `range()` already evaluates once).
- **An inclusive range may end at its type's maximum** (P40 review B6): `for i in 2147483646..=2147483647`
  runs twice on every target, never wrapping into an endless loop. Where the end is not a literal provably
  below the maximum, C# drives the loop with a wider counter (`for (long i__it = …; i__it <= n; i__it++) {
  var i = (int)i__it; … }`; `Int128`/`UInt128` for 64-bit loop types) and PHP stops a 64-bit counter once
  it overflows (`\is_int($i) && $i <= $n`); TS, Python and other PHP widths cannot overflow there.

**Reachability, returns and definite assignment (P40 review A9/B11).** One flow pass applies the C#/Java
reachability rules: `return`/`throw`/`break`/`continue` never complete; an `if` completes when either branch
does (an `if` without `else` always may); `while true` completes only through a `break` (any other loop may run
zero times, so it always may); `use` completes when its body does; a `try` completes when its body or any
catch does, and its `finally` does. Three errors follow, each a program the targets would otherwise treat
differently:
- **Unreachable code** — a statement after one that never completes (*unreachable code — … remove it*; C#
  CS0162). A trailing `return` after a `try` whose arms all return is therefore an error, not a tsc courtesy:
  the TS guard chain ends in an unconditional rethrow when no catch completes, so tsc proves the end too.
- **A missing return** — a value-returning function, method, getter or extension whose end is reachable
  (*… can reach the end of its body without returning a value*; C# CS0161, TS TS2366, while Python/PHP would
  silently return `None`/`null`). Iterators (`yield`) are exempt. A block-bodied lambda is held to the same
  rule when the function type it flows into has a concrete non-unit result (*this lambda can reach the end of
  its body without returning a value*; C# CS1643); a lambda whose result type is unknown or unit is not.
- **Definite assignment of class fields** — a field without an initializer must be assigned on **every**
  path through each `init` that completes, and before every early `return` in it (*… is not assigned on every
  path through init* / *`init` … returns before field 'x' is assigned*). Both `if` branches must assign; a
  throwing branch needs nothing; an assignment only inside a loop body is not definite. (C# CS8618 otherwise,
  and the field reads 0 / `undefined` / `AttributeError` / `null` per target.)
- **Writes to immutable members** — a `const` and a `static let` are never assignable (*cannot assign to
  immutable static field 'K.X' (declared with 'static let')*; C# CS0198, a PHP class `const`). A `let` instance
  field is assignable only through `this`, directly in a constructor of the class that declares it — not from
  a method, another receiver, a subclass or a lambda (C# `readonly`, CS0191). Inside a class, a static is
  named by its bare name for a write exactly as for a read: a local of that name first, then the enclosing
  class's static (`Counter = Counter + 1` is `K.Counter = K.Counter + 1`), compound operators included.
  A **record field** is never assignable (*cannot assign to record field 'P.x'*; build a copy with `with`).
- **`this` before `init` completes** (P40 review r3) — until every initializer-less field is assigned, `init`
  may only assign fields: no read of an unassigned field, no method call or computed property on `this`, no
  lambda capturing `this`, no passing `this` (Swift's two-phase rule). An instance field **initializer** may
  not use `this` at all (initializers run before the constructor on C#/TS and inside it on Python/PHP). The
  definite-assignment walk follows control flow exactly: a `while true` left by `break`, a do-while body and a
  `finally` that assigns before an early `return` all count. An inherited field counts as assigned (the base
  constructor already ran).
- **Initialization order** (P40 review r3, r4, r5, r6) — TS, Python and PHP run initializers in declaration
  order (C# alone initializes statics lazily, and reads a default), so an initializer is checked against
  everything it **runs**: its own expression, and transitively every body it reaches — functions and static
  methods it calls, constructors (with the class's instance field initializers and the base constructor),
  instance methods (and every override of them), computed property getters **and setters**, user operators and
  indexers (`a + b`, `a == b`, `-a`, `g[i]`, `g[i] = v`, a compound `a += b`, an explicit conversion), static
  computed properties, extensions, a function **or lambda passed as an argument** (the callee may call it at
  once), a lambda it invokes in place, a lambda **called through a local or a module value** bound to it, and the
  static initializers of every class it uses (defining a class runs them).
  - A **module value** may not reach a module value declared **at or after it in the same file** (*the
    initializer of 'a' calls 'seed', which reads 'b', declared later*; *… runs a lambda passed as an argument,
    which reads 'b', declared later*). A value whose initializer reaches **itself** is a cycle (*… uses 'K',
    which reads 'v' — 'v' reads itself during its own initialization*). It **may** reach a class declared after
    it: the emission order defines every class a value reaches — with its bases and the classes its own statics
    reach — before the value (P40 review r5: `let shop = Shop()` whose constructor builds a later `Item`).
  - An **enum, record, union or interface** is never an order problem, wherever it is declared (P40 review r6):
    the emission order places every declared type that is not a class ahead of all classes and values. Only
    classes are interleaved with values.
  - A **static** initializer may not read a static or module value declared at or after it, nor use (construct,
    read or call a static of) a class declared later, nor a class whose base is declared later. Code **outside**
    its class may not use the class while its statics run (*the initializer of 'Config.users' calls 'join',
    which reads 'Config' while class 'Config' is still being defined*): TS binds the class name only after the
    class is defined. A `static fn` of the same class may (it sees the class's own binding), and so may a lambda
    written inside the class.
  - Exempt: a lambda that is only **stored** (in a field, a collection, a module value that is never called)
    runs later, as do calls inside it; a function only stored as a value. A call through a local follows every
    lambda bound or assigned to it; a call through a module value follows the lambda it is initialized with (a
    `var` module value re-assigned inside a function is followed only into that initializer); a lambda that
    reaches a callee through a field or a collection is not followed (it is stored, not run, where the
    initializer hands it over).
  The targets that run a module top to bottom emit it in that dependency order: enums, interfaces (base
  first), unions, records, functions, extensions, then classes and module values in source order, a class
  moving earlier only for a value that reaches it or to follow a base declared after it (a module value can
  read an earlier one from a static initializer: `let SCALE = 2.0` then `static let FACTOR = SCALE * 3.0`).
- **Member hiding** — redeclaring an inherited instance member without `override` is refused (C# would hide it
  non-virtually, TS/Python/PHP override it); an `open` method takes `override`, anything else is renamed.
  Statics take part (P40 review r4): a static named like an inherited instance member, or the reverse, is a PHP
  fatal, and a static over an inherited static is C# CS0108 (an error under warnings-as-errors) and TS4114 —
  all refused; rename it.
- **Default parameter values** are constants: a literal (optionally negated), `null`, an enum case, or a float
  constant expression over `+ - * /`, which is folded to one literal (`dt: f64 = 1.0 / 60.0`). The fold follows
  the expression's types, as the same expression evaluates at run time: an integer-only part is `i32`
  arithmetic (`1 / 2 + 0.5` is `0.5`), widened where it meets a float; an `i32` overflow or an integer division
  by zero is not folded (and so refused). A negative zero keeps its sign, and an expression whose float literals
  are all `f32` folds in single precision to an `f32` literal (`0.1f + 0.2f` → `0.3f`). A default that is not a
  finite number (`-1e400`, an overflowing product, an `f32` product beyond single precision such as `3.0e38f * 10.0f`)
  is refused — no target can spell it as a constant. A call or
  any other expression is refused — C# and PHP accept only constants there, and Python evaluates a default once.
- **Integer literal range** — an unsuffixed integer literal (optionally negated) is an `i32` unless it flows into
  a 64-bit slot; one outside the range of the ≤32-bit integer slot it flows into, or of `i32` when it initializes
  an un-annotated local, is refused (`let v: i32 = 2147483648` — C# typed it `uint`). Suffix it (`2147483648i64`).
  The i64 minimum `-9223372036854775808` is a valid i64 literal on every target (PHP spells it `PHP_INT_MIN`).
- **Warnings-as-errors shapes** — a constant `if`/`while` condition (`if true`, `while 1 == 1`, and any condition
  built only from literals, operators and casts that C# folds: `if 1 + 1 == 3`, `if "a" == "a"`; `while true`
  itself is the infinite loop), a self-assignment `x = x`, `== null` on a non-nullable number, bool, char or
  enum value, and a local set to a constant (a literal, an operator or cast over literals, an enum member) and
  never read are refused (C# CS0162 / CS1717 / CS0472 / CS0219). CS0219 fires only when **every** assignment is
  a constant, so a local later assigned a non-constant (`var _last = 0; _last = compute()`) is accepted, read or
  not (P40 review r5). Prefix a deliberately unused local with `_`: such a `let _x = <constant>` that is never
  read has no effect and is not emitted (one later assigned only constants is refused — C# reports it whatever
  its name). An all-lowercase type name (`class point`) compiles under warnings-as-errors: the C# file disables
  CS8981 when one is declared.
- **Function values and operators** — any callee expression may be called (`adder(1)(4)`, `fs[0](4)`, an
  immediately-invoked lambda with typed parameters and an expression body), and so may a function-typed field
  (`h.f(4)`, `(h.f)(4)`, `K.g(4)`). A top-level function is a value of its function type (`apply(five)`; PHP
  passes the first-class callable `five(...)`) when it is a single, non-generic user function without default
  parameters — an overloaded, **generic** or **defaulted** (any parameter with a default) or std function is
  refused as a value (*function 'f' cannot be used as a value: it is generic*): pass a lambda that calls it
  (`(x: i32) => f(x)`). The value names the same target name a call would (its C# link name and PHP unique
  name — P40 review r5). A bare function name as a statement (`five`) is refused: write `five()`. A module
  value of function type is called by name like a local (`let twice = …; twice(4)`). A function
  value has no members and no methods (`g.toString()` is refused). A function value flowing into a function slot
  must have that type, with one relaxation (P40 review r5): **class/interface variance** — a parameter may be a
  SUPERtype of the slot's and the result a SUBtype (`(Animal) => string` where `(Dog) => string` is expected,
  `() => Dog` where `() => Animal` is). C# delegate variance (`Func<in T, out R>`; user classes and interfaces
  are reference types) and TS's function types accept exactly that; Python/PHP are untyped. Scalars (no variance
  for value types), records, nullability and type arguments must match exactly, and a value-returning function
  into `(…) => unit` is refused (C# CS0407): wrap it, `() => five()`. A generic call may not infer a type
  parameter as `unit` from a lambda that returns nothing (`apply<T>(f: () => T)` with `() => print("x")` — C#
  CS0411: there is no `Func<void>`); give the parameter a `(…) => unit` type, or return a value. A lambda as a
  binary operand is parenthesized on every target (`cb ?? (() => 9)`). A lambda flowing into a `(…) => unit` slot must not produce a value (a
  block's `return v`, or an expression body that is not a call); a call body's result is discarded on every
  target, also when the call is a bound property read or lookup (`() => s.len()` — C# `() => { _ = s.Length; }`).
  An expression statement whose value is dropped is spelled the same way. An `operator fn` is reached only
  through its operator, never called by name.
  `??` needs a left operand that can be null (a non-nullable or narrowed one is refused — the fallback is dead
  code, and C#/TS reject it). `>>>` and `>>>=` are refused: shift an unsigned value with `>>`.

**Self-comparison is refused (B11).** Comparing a side-effect-free path with itself — `x == x`, `a >= a`,
`E.A == E.A` — is constant except for NaN (C# CS1718). The NaN test is `Math.isNaN(x)` (std.math, `f64`;
an `f32` widens), on every target.

### 5.1 Exceptions (`try`/`catch`/`when`/`finally`, `throw`)

```pg
class ParseError : Error { constructor(msg: string) { super(msg) } }

fn parseTier(s: string): i32 {
  if s.isEmpty() { throw ParseError("empty tier") }
  return i32.parse(s)           // std string->int parse (static type method, §7) — not a cast
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
`if`-guard-then-rethrow; `finally` is identical on both. On PHP, `catch (e: Error)` (and an untyped
catch) catches `\Throwable`, so engine errors (`TypeError`, …) are catchable from `.pg` as well (P40 D13).
A catch binding the clause never reads (`catch (e: T)` with `e` unused, or `catch (_: T)`) is dropped from the
output — C# `catch (T)`, Python `except T:` — so no target warns about an unused exception variable.

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
therefore defines operators as **fixed-named methods**; the C# backend emits real `operator` members,
Python emits the native dunders, and TS emits **static-on-type** methods (`Money.plus(a, b)` — a static
travels with its type and, for `eq`, tolerates a null left operand where an instance call would throw).

| Operator | Method name | | Operator | Method name |
|---|---|---|---|---|
| `+` | `plus` | | `==` | `eq` |
| `-` (binary) | `minus` | | `<` `<=` | `lt` `le` |
| `*` | `times` | | `>` `>=` | `gt` `ge` |
| `/` | `div` | | `-` (unary) | `neg` |
| `%` | `rem` | | `&` `\|` `^` | `band` `bor` `bxor` |
| `<<` `>>` | `shl` `shr` | | `~` (unary) | `bnot` |
| *(hash)* | `hash` — required with `eq` | | | |

Any other `operator fn` name is an error (*'x' is not an operator*).

**A custom `eq` requires an author `hash`** (P40 review A9): `operator fn hash(): i32`, an instance member
hashing exactly the state `eq` compares, so values equal under `eq` hash equally. It becomes C#
`override GetHashCode()` (next to `Equals(T?)`, `operator ==(T?, T?)`), Python `__hash__`, and a `hash()`
method on TS/PHP. No compiler can derive a hash from an arbitrary `eq` body — the generated structural hash
would disagree with it (a C# `HashSet`/`Dictionary`, a Python `set`/`dict` then misbehave) and a constant one
is consistent but degenerate — so a type declaring `eq` without `hash` (or `hash` without `eq`) is refused.

```pg
record Money(cents: i64) {
  operator fn plus(o: Money): Money => Money(this.cents + o.cents)
  operator fn lt(o: Money): bool => this.cents < o.cents
  operator fn explicit(): f64 => (f64)this.cents / 100.0   // explicit conversion: (f64)m
}
// a + b   -> C#: a + b          |  TS: Money.plus(a, b)  |  Python: a + b (__add__)
// a < b   -> C#: a < b          |  TS: Money.lt(a, b)
// (f64)m  -> C#: (double)m      |  TS/Python/PHP: m.toF64()
```

`eq`/`lt` also feed `match` literal/comparison patterns and the generated structural equality.
Comparing against the **`null` literal is always a null test** — it never routes to a user `eq`.
**Explicit conversions** are declared `operator fn explicit(): T` (the return type IS the conversion
target; no parameters); C# emits a native `explicit operator`, method-form targets generate `to<T>()`
and rewrite the cast site. **Implicit** user conversions are refused (invisible call-site injection).
`string`/`bool` conversion targets are refused (toString/truthiness semantics differ per target).
**Compound assignment** on a user type lowers to `target = <op>(target, value)`; through a user indexer
it becomes the get+set pair; the receiver must be a simple name/`this` (hoist a computed receiver to a
local — single evaluation by construction).
On a **built-in** slot (a local, field, static or element of a scalar or `string` type), `t op= v` means
exactly `t = t op v` — the same §3.C wrap, integer division/modulo truncation and concatenation as the
binary operator — with `t`'s receiver and indices evaluated **once**, and `v` converts into `t`'s type as
in a plain assignment. A target whose native compound already means that (the plugin trait
`nativeCompoundAssign`; C#) keeps it; elsewhere the compound expands, an impure receiver/index goes to a
temp first, and the output folds back to `op=` only where the binary op needs no wrap or helper.

**Operands are evaluated left to right** on every target — a binary operator's left operand (and a compound's
target) is read before its right operand runs, as C#, JS and Python do. PHP reads a *variable* operand only
when the operator executes (`$n + bump()` saw `bump()`'s write to `$n`; so did `$n += bump()` and a native
`$this->total += $this->next()`). A target declaring the plugin datum `operandSnapshot` (PHP) snapshots such
a variable into a temp when the right operand may have side effects (`($__c3 = $n) + bump()`) and keeps an
impure compound expanded (P40 review r4).

Operator support is **graded per target** via keyed capabilities
`operatorOverloading:{arithmetic,comparison,eq,indexers,conversion}`: PHP supports `:eq` (`->eq(...)`)
and `:indexers` (`->get`/`->set`) while refusing the rest; C#/TS/Python support all grades.

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

A computed property may be **static** (`static let level: i32 => Cfg.base * 2`, P40 review r6): C# and TS emit
a static property / static getter; Python and PHP emit a static method that every read calls (`Cfg.level()` /
`Cfg::level()`). A static property **setter** is native on C#/TS and refused on Python (capability
`propertySetters:static` — a class-level assignment would bypass any descriptor) and on PHP (no setters).

### 6.3 Extension functions

```pg
extension fn string.shout(): string => this.toUpper() + "!"
extension fn List<T>.second(): T? => if this.count >= 2 { this[1] } else { null }
// "hi".shout()  ->  C#: static extension call  |  TS: shout("hi")
```

An extension may declare trailing **default parameters** (`extension fn string.indexOf(needle: string,
from: i32 = 0)`); a call may omit them, and the default is **filled in at the call** on every target —
for a bound (`actual … extern`) extension that is the only way its template's `$1` gets a value. Because the
default is evaluated in the caller's scope, it must be a literal (optionally negated) or `null`. The same
holds for a member of an `extern class`.

### 6.4 Lambdas & closures

```pg
let double  = x => x * 2              // single untyped parameter: parentheses optional
let add     = (a, b) => a + b         // zero or 2+ params need parens: () => …, (a, b) => …
let typed   = (x: i32) => x + 1       // a type annotation needs parens too
let counter = (n: i32) => { total += n }   // block body; closes over `total`
```

A lambda lowers to a C# lambda (`x => x * 2`) and a TS arrow function (`x => x * 2`) — the arrow
spelling is shared, so this is a pass-through with no per-target rewrite. Captures are by reference on
both targets (PRD §3.A). The **bare single-parameter form is the only place parentheses are optional**;
the canonical formatter prints `x => …` for one untyped parameter and parenthesizes everything else.
Note `=>` is reused for expression-bodied members (§6.1/§6.2) and `match` arms (§4.5) — those are not
lambdas.

**Loop-variable capture is per-iteration** (issue #46): a closure created inside a `for` loop captures the
loop variable's value *at that iteration*, not a single shared variable. This is the TS `let` / PHP
`use($i)` semantic and matches C#'s own `foreach` (since C# 5); it is the least surprising choice. Targets
whose native `for` differs get a capture shim: C# copies the range binding into a per-iteration local
(`for (var i__it = …) { var i = i__it; … }`), and Python snapshots it as a default argument
(`lambda …, i=i: …`). So `for i in 0..3 { fns.add(() => i) }` yields closures returning 0, 1, 2 on every
target.

**A lambda reads a module value when it runs** (P40 review r5), not when it is created — the exemption §5
grants stored lambdas from the initialization-order rule depends on it. PHP arrow functions capture by value
at creation, so a module value read inside any lambda is emitted as a live global read there
(`$GLOBALS['later']`); the other targets' closures already read the binding.

**Assignment order.** A plain `t = v` names its slot before `v` runs, on every target: `xs[j] = incJ()` writes
`xs` at the old `j`, `d.v = swapD()` the object `d` held before. Python and PHP evaluate the right side first
(manifest `assignmentOrder: "valueFirst"`), so when `v` may have side effects, the target's receiver and indices
that a call could change — a `var` some lambda assigns, a field read, a call — are evaluated into temps ahead
of it there (P40 review r5).

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
1:1. `char` is a single UTF-16 code unit (a `number` in TS, `char` in C#). A char literal therefore holds
exactly one character at or below U+FFFF: `'😀'` / `'\u{1F600}'` is an error (*a char is one UTF-16 code
unit — use a string*), as are `''` and `'ab'`. Code points outside the BMP are
surrogate pairs; the std string API exposes code-point iteration for correct handling, and the spec warns
(as C# and JS both do) that indexing yields code *units*, not code points. String interpolation
`"hi ${name}"` lowers to C# `$"hi {name}"` and TS template literals — both first-class on each target.
String `==` is value equality on both targets (C# `string ==`, TS `===`).

**Number text — one rule on every target (P40 A3).** `print`, an interpolation hole and anything else that
turns a number into text follow one rule:
- integers render as **invariant decimal** (`-5`, never a culture's minus sign or digit grouping);
- `f64` renders as **ECMAScript `Number::toString`**: the shortest digits that round-trip, plain notation for
  `1e-7 <= |x| < 1e21` and `e±N` otherwise (`1e+21`, `1e-7`), `-0` as `0`, and `NaN`, `Infinity`,
  `-Infinity`;
- `f32` renders as its **exact value widened to `f64`**, under the `f64` rule: `0.1f` prints
  `0.10000000149011612` everywhere (an `f32` literal denotes the nearest `f32` on every target);
- a `null` reaching a type-parameter hole renders `null`.

So `0.1 + 0.2` prints `0.30000000000000004` and `1e20` prints `100000000000000000000` on C#, TS, Python and
PHP alike, independent of the machine's culture (C# number text goes through a culture-free, file-local
`__PgRt` helper; the generated `Main` no longer pins a culture) and of PHP's `precision` settings. Parsing is
culture-free too (`i32.parse("-5")`, `f64.parse("1e21")`).

**What can be printed.** `print` and an interpolation hole accept exactly the same types: the integer and
float scalars, `bool`, `char`, `string`, and a type parameter. Everything else is a compile error with no
portable text — a record, class, union, list or other collection prints differently on every target. An
**enum** is refused too: C# prints the case name and the other targets its number, so map it to a string
explicitly (a `match` returning the text you want).

**Case mapping.** `toUpperAscii()`/`toLowerAscii()` change only `a`–`z`/`A`–`Z` and keep every other
character — identical on every target. `toUpper()`/`toLower()` are the published **best-effort** tier (§9).

**The string std (`import "std.strings"`, P40 B3).** Every operation is **ordinal** (no culture, no locale:
C# binds the `StringComparison.Ordinal` overloads), every index is a **UTF-16 code unit**, and every argument
out of range throws an `Error` with the same message on every target:

| Function | Result | Throws `Error` when |
|---|---|---|
| `s.len()` | the length in UTF-16 code units (`"😀".len()` is 2) | — |
| `s.charAt(i)` | the unit at `i`, as a one-unit string | `i < 0` or `i >= len` |
| `s.codePointAt(i)` | the code point starting at `i`; a lone surrogate yields its unit | `i < 0` or `i >= len` |
| `s.substring(start, end)` | units `[start, end)` (may split a surrogate pair) | `start < 0`, `end > len` or `start > end` |
| `s.indexOf(needle, from = 0)` | the first unit index `>= from`, else `-1`; an empty needle finds `from` | `from < 0` or `from > len` |
| `s.startsWith(p)`, `s.endsWith(p)`, `s.contains(n)` | ordinal tests | — |
| `s.replaceAll(needle, replacement)` | every non-overlapping match replaced literally (`$$`/`$&` mean nothing) | `needle` is empty |
| `s.split(sep)` | a `List<string>`, empty pieces kept (`"a,,b"` → 3) | `sep` is empty |
| `s.trimAscii()` | strips exactly U+0009–U+000D and U+0020 at both ends (not GS, NBSP or U+FEFF) | — |
| `string.fromCodePoint(cp)` | the code point as a string (2 units when astral) | a surrogate, `cp < 0` or `cp > 0x10FFFF` |
| `compareOrdinal(a, b)` | `-1`/`0`/`1` by UTF-16 code unit (never numeric: `"10"` < `"9"`) | — |

The checks are written **once**, in `.pg`, inside `std.strings` itself, over unchecked per-target primitives
— so the four plugins cannot drift. The primitives (`__pg…`) and the emitted guards (`_pg_…`) use prefixes
user code may not declare or reference. `split` returning `List<string>` does not make `List` visible to a
caller that never imported `std.collections` (§11: an import is visible only to the module that writes it).

**Index units on Python and PHP.** C# and TS strings are UTF-16 natively. Python holds code points and PHP
UTF-8 bytes, so their string std counts through helpers instead (capability `utf16Strings:index`,
"emulated"): an ASCII-only string takes a fast path (Python `str.isascii()`, a PHP byte scan); any other is
indexed through a cached UTF-16 view (Python: astral code points expanded to surrogate pairs; PHP: an
**iconv** UTF-16LE copy — **the PHP target requires `ext-iconv`**, and names it in the `Error` it throws
when a non-ASCII string is indexed without it). The searches (`contains`, `startsWith`, `endsWith`,
`replaceAll`, `split`) use the native code-point / byte operation — which gives the UTF-16 answer for any
well-formed operands — and switch to the same UTF-16 view as `indexOf` when an operand holds a lone surrogate
(P40 review B2), so a lone half never matches differently from one search to the next or from C#/TS.
Printing a lone surrogate writes U+FFFD on every target.

---

## 9. Faithful-by-default & the published relaxation list (PRD §3.C)

These are the points where C# and JS genuinely differ. Default behaviour is the faithful one; each
relaxation is **opt-in and named**, never silent:

| Concern | Default (faithful) | Relaxation (opt-in / documented) |
|---|---|---|
| `i32`/`u32`/sub-32 overflow | masked at op boundaries in TS (`\| 0`, `>>> 0`, `Math.imul`) so JS wraps like .NET | intermediate values below 2⁵³ that .NET would wrap are *not* re-masked unless typed `i32` — made explicit, not silent |
| `i64`/`u64` | TS `bigint` (correct, slower) | a two-word `Long` class may be offered later if `bigint` is too slow |
| `f32` vs `f64` | `f32` rides a JS `number` (double); an `f32` literal is the nearest `f32` on every target, and `f32` **text** is faithful (the exact value widened to `f64`, §8) | `f32` **arithmetic** is relaxed: TS, Python and PHP compute it in double precision. Per-op `Math.fround` strict-float mode is opt-in (the Scala.js strict-floats tax) |
| number text | **faithful**: integers invariant decimal, `f64` as ECMAScript `Number::toString`, culture- and ini-independent (§8) | — |
| `toUpper`/`toLower` | **published best-effort tier:** JS and Python map full Unicode (`ß`→`SS`, final sigma), C# uses the invariant simple mapping, PHP is ASCII-only. ASCII text is identical everywhere | use the faithful `toUpperAscii`/`toLowerAscii` when identical output matters |
| nullability | `null`/`undefined` normalized to one "absent" | — |
| equality / hashing | `record` gets structural `==`/hash; `class` is identity; identity hash via side `WeakMap` in TS | — |
| record fields in `==` | leaves compare strictly per their own `==`: value types and strings by value, `List<T>`/`T[]`/`Map<K, V>`/class fields by REFERENCE (the .NET default field comparer); nested records recurse structurally | — |
| `Math.round` half values | **half-to-even** (banker's — the .NET default) on every target; TS and PHP carry shims (their natives are half-up / half-away) | — |
| shifts | count masked (`& 31` / `& 63`, the .NET rule) and result wrapped to the operand width on every target | — |
| float `%` | C-style `fmod` (result takes the DIVIDEND's sign — the .NET/JS behavior); Python emits `math.fmod`, PHP `fmod()` | — |
| PHP 64-bit wrap | **published caveat:** PHP has no i64/u64 wrap emulation (no GMP dependency) — i64 arithmetic is faithful only while intermediates stay inside ±2⁶³ (beyond it PHP promotes to float, so an overflowing i64 product prints as a float such as `9223372036854776000` where C# prints the wrapped value); u64 values above 2⁶³−1 are not representable | a GMP-backed strict mode could be offered later |
| string index units | **faithful**: `len`/`charAt`/`codePointAt`/`substring`/`indexOf` count **UTF-16 code units** on every target — natively on C#/TS, through helpers on Python and PHP (§8; PHP requires `ext-iconv`). `"é".len()` is 1 and `"a😀b".len()` is 4 everywhere; `compareOrdinal` orders by code unit everywhere (an astral character sorts before U+E000–U+FFFF) | — |
| lone surrogates | a `substring`/`charAt` that splits a pair holds the lone half faithfully (its `len`, `codePointAt` and printed U+FFFD agree everywhere), and every std search matches it by UTF-16 code unit on every target: `indexOf`, `contains`, `startsWith`, `endsWith`, `replaceAll` and `split` agree (`"a\u{1F600}".contains(hi)` is true and `split(hi)` gives 2 parts on all four; Python/PHP switch to the UTF-16 view only when an operand holds a lone half) | **published caveat:** on Python and PHP, two lone halves joined by `+` are not re-paired for `==` (`"\u{1F600}".substring(0, 1) + "\u{1F600}".substring(1, 2) == "\u{1F600}"` is true on C#/TS, false on Python/PHP); `print` still shows the paired character |
| `codePoints()` | iterates code points on every target | **published caveat:** a lone surrogate is U+FFFD on C# (`EnumerateRunes`) and kept as is on TS/Python; PHP's `preg_split('//u')` fails on the ill-formed string |

**Determinism honesty (PRD §3.D):** only `+ − × ÷ √` are reproducible across .NET and JS (at matched
width). Transcendentals (`sin`/`cos`/`exp`/`pow`), FMA, and reassociation diverge. Code that needs
identical cross-target results must use the std **fixed-point / soft-float** type (a P7 std module), *not*
`f32`/`f64`. The FruitCake sketch uses only `+ − × ÷ √`, so its eventual conformance test gates on
tolerance + behaviour, never bit-equality.

Two integer edges are §3.D territory as well (documented, deliberately unpinned):
- **`INT_MIN / -1`** — .NET throws `OverflowException`; the wrap targets yield `INT_MIN`. Don't rely on it.
- **out-of-range float→int casts** — trunc-then-wrap on the wrap targets, target-defined on .NET
  (the CLR conversion is unspecified out of range). In-range casts truncate toward zero everywhere
  (pinned by `cast_float_int.pg`).

(Scalar `parse` on out-of-range or garbage input is **not** in this list: one strict grammar, checked in `.pg`,
throws the same `Error` on every target — §3, pinned by `parse_strict.pg`.)

---

## 10. Refused — not in the grammar (PRD §3.B)

These have **no production** in [`grammar.ebnf`](grammar.ebnf). You cannot write them; if you reach for the
underlying platform API anyway, the type/name simply does not resolve, and P6 adds a *targeted* refusal
diagnostic (e.g. naming `Thread` or `decimal`) so the message is "Polyglot refuses X because …", never a
miscompile.

- **Threads / concurrency primitives** — `lock`, `Interlocked`, `Parallel.*`, thread spawning. Single-
  threaded `async`/`await` only (design in PRD §4.7 — a colored function, author writes the unwrapped `T`,
  per-backend wrapper (`Task<T>`/`Promise<T>`/`async def`), capability-gated; **implemented 2026-07-01**).
- **Runtime reflection** — `Activator`, open-world `Type.GetType(string)`, and **native attribute
  introspection at run time** (`GetCustomAttributes`, `ReflectionAttribute::newInstance`,
  reflect-metadata). (Defeats tree-shaking; #1 bloat source.) The three-way attribute split (P37 §D):
  native runtime reflection is refused, forever; **Tier 1** `extern attribute` pass-through is allowed
  (Polyglot emits the native annotation for the target's OWN framework and never reads it back); **Tier 2**
  `attribute` + `Meta` queries are allowed **because nothing resolves at run time** — the lookup happens
  at transpile time and the output carries only constants (§4.7). Runtime-variant `Meta` queries
  (`Meta.of(expr)`, type-parameter type args, computed member names) are refused **permanently** (M6):
  they would require shipping per-type metadata registries plus a uniform runtime type identity that the
  four targets do not share.
- **Behavior-transforming decorators** (Tier 3) — an attribute is never executable code: TS/Python
  decorators RUN at definition time while C#/PHP attributes never run, so a portable behavior-injecting
  attribute cannot exist (P37 §D.1). If a specific transform is ever wanted, it becomes a fixed
  first-party compiler lowering — never a user-programmable macro.
- **Finalizers / GC hooks** — no `~T()`, no `GC.*`. Deterministic disposal is `use` (§5.2) — that's all.
- **Base-member access through `super`** — `super.m()`, `super.field` and a bare `super` value are refused
  (P40 D6). The only `super` is the base-constructor call `super(…)` as a top-level statement of a
  constructor. (PHP's `parent::` access differs from the other targets' `super.`/`base.`; supporting it
  would need its own IR node, so it waits for real demand.)
- **`decimal`** — no `decimal` type or literal (a big-decimal std type may be opted in later).
- **`unsafe` / pointers** — no `unsafe`, `*T`, `&`-address-of, `stackalloc`, raw `Span`.
- **`dynamic` / runtime code-gen** — no `dynamic`, `Reflection.Emit`, `Expression.Compile`.
- **LINQ expression trees** — no `Expression<Func<…>>`. A lambda (§6.4) is *always* an executable closure
  (→ C# delegate, TS arrow function), never code-as-inspectable-data: JS has no portable way to recover a
  typed AST from a function at run time, so there is nothing to lower a queryable tree to.
- **Bit-exact cross-target floats** — not promised; see §9 / PRD §3.D.

### 10.1 Issue-sweep (P36) scope decisions

These clarify or bound features surfaced by the wave-2 coverage sweep (issues #38–#57):

- **Typed `match` patterns / type tests** (#38, relaxed by P37 B4) — a typed arm `x: Circle` IS a runtime
  type test on a concrete **class or union** (C# declaration pattern / TS·PHP `instanceof` / Python
  `isinstance`), narrowing the binding. A type test against an **interface** gates per-target
  (`interfaces:runtimeIdentity`): refused iff a configured target lacks runtime interface identity (TS
  erases interfaces); a C#/Python/PHP-only build allows it.
- **Union `==`** (#57) — **structural** (compares tag + payload), like records; documented in §9.
- **A user `operator fn eq`** (#49) overrides the default structural/reference equality on every target;
  it must come with a matching `operator fn hash(): i32` (§6.1, P40 review A9).
- **Per-iteration loop-variable capture** (#46) — see §6.4.
- **Block-bodied `match` arms** are refused (#51) — a match arm must be an expression (Python/PHP lower
  `match` to an expression fold with no statement block). Extract the block into a function.
- **Member method overloading** is refused (#53) — `fn` overloading is **top-level only**; a duplicate
  same-named method is a declaration-site error. Give the overloads distinct names. Across modules
  (P40 review A8), a call only considers the overloads **visible to the calling module** (its own and the
  ones its imports name): another module's never-imported `check(i32)` cannot win `check(5)`. Two modules
  declaring the same signature is a duplicate, and the error names both modules. An argument applies to a
  parameter only when it converts: a union or enum parameter takes only that type, a declared type never takes
  a scalar (P40 review r5 — `kind(5)` had bound `kind(s: Shape)`). C# links every module's functions into one
  class, so two declarations C# would see as one signature — type parameters differing only in name
  (`same<T>(T)` / `same<U>(U)`), reference types differing only in nullability (`describe(string)` /
  `describe(string?)`) — get signature link names (`same__T`, `describe__stringOrNull`) whether or not
  anything calls them; a lone declaration keeps its name.
- **Namespace and renaming imports** are refused (#39e) — `import * as ns` and `{ a as b }`; import members
  by their own name (`import { a, b } from "…"`). Modules merge into one flat namespace.
- **An unresolvable member on a concrete/primitive receiver** is refused, not emitted verbatim (#56a) —
  usually a missing `import`.

---

## 11. Module structure & imports

A `.pg` file is one compilation unit. Top-level declarations are module-public (a `private` opt-out marker
is planned, not yet implemented). Imports are **TypeScript-style**: a named group, namespace, or bare form,
with the module given as a **quoted string specifier**:

```pg
import { sqrt, PI } from "std.math"
import { readText, writeText as wt } from "std.io"   // `as` rebinds an imported name
import * as coll from "std.collections"              // namespace import
import "std.io"                                       // bare: link only
```

`from` and `as` are **contextual identifiers**, not reserved words (so they remain usable as ordinary
names). The specifier is a quoted string so that a bare name like `"std.io"` reads as a logical module and a
`"./physics"` reads as importer-relative (Node/TS/Deno convention) — the latter enables cross-`.pg`
user modules. An interpolated string (`"…${}…"`) is rejected as a specifier.

**Target names a user name may collide with.** A user identifier that is a target keyword is respelled on that
target (C# `@switch`, Python/PHP `global_`) or, on a target with no escape (TS), refused. The generated code
**qualifies the runtime names it uses** wherever the target allows it, so a user name that merely spells one keeps
its name (P40 review r5): TS throws `globalThis.TypeError`/`globalThis.Error` and reaches `globalThis.process`;
Python calls every builtin through the `builtins` module (`builtins.len`, `builtins.range`, `builtins.isinstance(x,
builtins.bool)`, `@builtins.property`; `import builtins` is emitted only when used) and imports `abc`/`asyncio` under
compiler names (`_PG_abc`), so a function `max`, a field `id`, a parameter `len` or a local `str` is emitted as
written — only the name `builtins` itself is escaped (`builtins_`), and a user name `Exception` (the spelling of
std's `Error`) is refused; PHP names `\Exception`. Where a name cannot be qualified, the escape applies only to the
binding kind that collides: on PHP a builtin **function** name is suffixed only for a free function (`fn intdiv` →
`intdiv_pg`; a method or field `intdiv` keeps its name), and a builtin **class** name only where a type name is
spelled (`class Directory`/`Generator`/`TypeError` → `Directory_`, also in `new`, `::` and type positions; a field
`directory` or `error` keeps its name). A TS runtime global the output cannot qualify everywhere (`Math`, `Number`,
`String`, `BigInt`, `Array`, `JSON`, `Promise`, `console`, `globalThis`) is refused as a user name. A unit check
scans every manifest's templates and prelude for unqualified runtime references the lists do not cover, and for any
bare Python builtin, so this cannot drift. PHP refuses to redeclare **any** builtin function, used or not; the
list covers the ones the output calls and the common names — a user function named like another PHP builtin is
still a PHP fatal (rename it). On C#, a user `_` is respelled `__u` (`identifiers.renames`):
the output writes a dropped value as the discard `_ = …`, which a variable named `_` would capture. A user type
may not reuse a **core type** name (`Error`, `Iterable`, `Array`, `Option`, `INumber`): std code relies on them
(*'Error' redeclares the std core type 'Error'*). A respelled name is part of that target's public surface —
pick another name for an exported API.

**Resolution** (PRD §4.5, PLAN P12): `std.*` resolves from the compiler's embedded registry; other
specifiers resolve through a pluggable module loader (the CLI maps a logical `a.b.c` to `<root>/a/b/c.pg`
and a `./x` relative to the importing file; cross-file user-module loading is the P12 deliverable).

**Visibility (P40 D16).** An import makes names visible **only to the module that writes it**: a bare
import (`import "./x"`, `import "std.strings"`) every name of that module, a selective one only the names it
lists. A module's own imports do not leak to the modules importing it — `std.strings` imports
`std.collections` for `split`'s `List<string>`, yet a caller that names `List` imports `std.collections`
itself (using the returned list without naming its type needs no import). The `lib` prelude
(`pgconfig.json` / `--lib`) is visible everywhere, and `std.core` (`Error`, `Option`, `Iterable`, …) always
is. A name used without the import that makes it visible is a compile error naming the import to add. The
merged closure still shares one namespace, so two modules of one closure may not declare the same name
(a collision is an error, never a silent pick) — per-module namespaces are a later step. A bare import emits
real target imports for every name (TS `import { … }`, Python `from x import *`), so an extension-only module
links. Renaming (`as`) and namespace (`* as ns`) imports are refused (PRD §3.B, #39e). `expect`/`actual` (target-gated capabilities) and `extern` (the FFI hatch) are implemented as
of P7.

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
