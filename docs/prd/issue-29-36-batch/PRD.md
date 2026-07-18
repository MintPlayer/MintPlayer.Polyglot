# Open-issue batch: #29 interfaces (gaps) · #33 hex lexing · #34 char-ordinal · #35 PHP globals · #36 PHP list semantics (PRD)

> **GitHub:** resolves **all five open issues** —
> [#29](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/29) (interface / abstract-method support),
> [#33](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/33) (hex literals truncated),
> [#34](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/34) (no char-to-ordinal path),
> [#35](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/35) (PHP module globals invisible),
> [#36](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/36) (PHP `List<T>` value semantics) —
> **in a single pull request** (maintainer decision), plus a README link to the QR-code demo repo
> (<https://github.com/PieterjanDeClippel/qrcode-demo>), the downstream project that surfaced #33–#36.

- **Status:** Draft v1.0 · 2026-07-18 · ships as a lockstep **minor** (new std surface + checker
  tightening + Python interface emission) — the PR carries `release:minor`; versions stay build-injected
  `0.0.0-dev` placeholders (P24), no in-tree bump.
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** a **6-agent read-only team investigation** (one agent per issue + one test-infrastructure
  survey), each verified against the source at HEAD (`0e53dc0`) and, for #29/#35, by *running* the compiler
  on the issues' repros. Findings folded in below with `file:line`.
- **Reporter context:** #33–#36 were found live while writing a QR encoder in Polyglot (the demo repo
  above); #29 while single-sourcing an AlphaZero chess engine (two neural nets behind one MCTS driver).

---

## 1. What the investigation actually found (it differs from the issues)

Reading the five issues at face value would have produced the wrong plan. The team verified each against
HEAD and three of them shift materially:

| Issue | As reported | As verified at HEAD |
|---|---|---|
| **#33** hex | "trailing lowercase `f`/`d` eaten as float suffix" | Correct — plus a **worse unreported case**: `0xf32`/`0xf64` strip the whole 3-char float suffix → the **invalid literal `0x`**. Root cause is split across two stages (§2.1). |
| **#34** char | "`charAt` unmapped on Python (emitted verbatim); `(i32)` cast dropped" | `charAt` **is** mapped on all four targets (Python emits valid `$this[$0]`) and returns **`string`**, not char — `(i32)charAt(…)` is *already* diagnosed. The live miscompile is `(i32)` on a **char literal**: the MVP scalar lattice has no `Char`, so both cast guards never fire (§2.2). |
| **#35** PHP globals | "functions referencing module `let` lack `global`" | The issue's literal repro (free function) was **already fixed by P26** — verified green at HEAD. The still-live bug is **class/record methods and property getters**, which never got the `scanGlobalRefs` treatment (§2.3). |
| **#36** PHP lists | "list *parameters* copy — emit `&$xs` / ArrayObject / refuse mutation" | Verified — and **plain aliasing diverges too** (`var b = a; b.add(x)` leaves `a` untouched on PHP only). `&`-params would fix one symptom and leave the class silently broken (and PHP fatals on `f([1,2,3])` by-ref). ArrayObject is the only fix that closes the class (§2.4). |
| **#29** interfaces | "add interface support" (full feature request) | **~70% already shipped** (P14b/P19): `interface` decls, `class X : IFoo`, multiple interfaces, interface inheritance, interface-typed params/fields/returns/locals, and virtual dispatch all work **today on C#, TS, and PHP** — verified by running the issue's exact MCTS shape. The real gaps: Python **miscompiles** (emits the implements clause with no interface class → `NameError`), the checker never verifies a class actually implements its interfaces, nominal assignability is absent, and an adjacent `match` typed-pattern miscompile (§2.5). |

The common thread: **four of the five are §3.B-class silent miscompiles or silent divergences** — same
source, different runtime results — which the PRD's scope contract flags as the worst defect class. This
batch is therefore a *faithfulness* milestone, not a feature milestone (interfaces' remaining work
included: its gaps are all "make the already-shipped feature honest").

---

## 2. Current reality + design, per issue

### 2.1 · #33 — hex literals: accidental tokenization + radix-blind suffix stripping

**Mechanics (two stages).** The lexer has **no hex branch at all** — `0xff` tokenizes by accident:
`readDigits` (`lexer.cpp:169`) reads `0`, stops at `x`, and the generic width-suffix loop
(`lexer.cpp:187-193`) greedily re-appends `xff`, yielding an `IntLit` with intact text. The truncation
happens later in lowering: `stripNumericSuffix` (`lower.cpp:20-27`, call site `:459`) matches suffixes
against `{i8…u64, f32, f64, f, d}` on the *final characters of the text*, so `"0xff"` matches `"f"` →
`"0xf"`, `"0x11d"` matches `"d"` → `"0x11"`, and **`"0xf32"` matches `"f32"` → `"0x"`** (invalid). The
emitter prints literal text verbatim (`emitter_base.cpp:225`) on every target — a Core bug, uniform
across all four. Uppercase hex, binary/octal alphabets, and genuine integer suffixes (`0xffi64`) are safe
by luck.

**Design — both halves, per the principled-fix rule:**
1. **Real radix lexing** (`lexer.cpp` ~166): on `0x`/`0X` (and `0b`/`0o` for symmetry), scan
   base-appropriate digits, then accept **only integer** width suffixes (`i8`…`u64`) — no target has hex
   floats. Enables hex-digit validation as a bonus.
2. **Radix-aware suffix stripping** (`lower.cpp:20`): text starting `0x`/`0b`/`0o` (any case) never has
   `f`/`d`/`f32`/`f64` stripped. This is the change that stops the observable truncation; (1) removes the
   fragile accidental tokenization and the `0xf32 → 0x` class at the source.

### 2.2 · #34 — the char-to-ordinal path

**Mechanics.** `std.strings` (`compiler.cpp:159-174`) declares `charAt(index: i32): string` — mapped on
all four plugins (C# `$this[$0].ToString()`, TS `$this.charAt($0)`, Python `$this[$0]`, PHP
`substr($this,$0,1)`), so the issue's "unmapped on Python" bullet is stale. The genuine miscompile is a
**char** value (produced only by a char literal `'A'`) under `(i32)`: the MVP scalar lattice `Ty`
(`ast.hpp:17`) has no `Char`, so `scalarTyOf(char)` → `Unknown` and neither cast guard
(`sema.cpp:1080-1087`, `:1318-1320`) fires. Per target: C# `(int)('A')` = 65 ✓ (by accident); TS
`("A" | 0)` = 0 ✗; Python **drops the cast** ✗; PHP `(int)"A"` = 0 ✗.

**Design — define the problem away, then diagnose the leftover:**
1. **Add `codePointAt(index: i32): i32` to `std.strings`** — the portable ordinal accessor the issue asks
   for. One line in `STD_STRINGS` + one arm per plugin beside `charAt`:
   C# `((int)$this[$0])` · TS `$this.codePointAt($0)!` · Python `ord($this[$0])` · PHP
   `mb_ord(mb_substr($this, $0, 1))` (multibyte-correct; the existing byte-based `charAt` is left alone).
   `"A".codePointAt(0)` → `65`, byte-identical on all four.
2. **Make `(i32)char` a diagnostic** — name-based check at the two guard sites: *"cannot cast char to a
   numeric type — use s.codePointAt(i) for the ordinal"*. This follows the issue's own framing (a cast
   that can't be honored portably must refuse, never miscompile). **Rejected alternative:** honoring the
   cast by lowering `(i32)char` to per-target ordinals — more emitter surface (a new node attribute + a
   branch in all four Cast templates) for an idiom `codePointAt` already covers. `(i32)string` stays
   diagnosed (already is).

### 2.3 · #35 — PHP `global` for methods and property getters

**Mechanics.** P26 built the machinery — `scanGlobalRefs` free-variable analysis, `ir::Function.globalRefs`
(`ir.hpp:412`), `FnDeclCtx` exposure (`emitter_base.cpp:947-950`), the `global $x;` prologue in `phpFnBody`
(`plugins/php/…:1863-1930`) — but wired it **only for free functions and extensions** (`lower.cpp:224,242`).
`ir::Method` has no `globalRefs` field, `Lowerer::method()` (`lower.cpp:430`) never scans, `MethodDeclCtx`
exposes nothing, and the PHP `MethodDecl` template (`plugins/php/…:2027-2062`) has no prologue. Verified
live: free fn ✓, class method ✗, record property getter ✗, closure-inside-method ✗ (same root cause).

**Python is not affected, and no write-case exists anywhere:** module-level bindings are `let`/`const`
only (no module `var` — the parser rejects it), so a module global can never be *assigned* from a
function; PHP needs `global` for reads (this bug), Python only for writes (impossible). The conservative
shadowing rule carries over: worst case is a *loud* PHP warning the differential gate catches, never a
silent miscompile.

**Design — extend the proven mechanism, don't add a second one:** `globalRefs` on `ir::Method` (mirror
`ir::Function`); hoist the module-global name-set to a `Lowerer` member (the record/class loops run before
the current local set is built) and scan in `method()` — **including `exprBody`, so property getters are
covered**; expose `decl.globalRefs` from `MethodDeclCtx`; factor the PHP `global` prologue into a shared
macro invoked by both `phpFnBody` and `MethodDecl`. Rejected: `const`/`define()` hoisting (can't represent
the array-typed `let TABLE`); static-class wrapper (heavier redesign, inconsistent with the shipped path).

### 2.4 · #36 — PHP reference semantics for `List<T>` and `T[]`

**Mechanics.** `List.type: "array"` (`plugins/php/…:159-166`) maps to a native PHP array — copy-on-write,
value-typed. The divergence is **not just parameters**: `var b = a` emits `$b = $a` (a copy), so aliasing,
lists-in-tuples, and returned-then-aliased lists all silently diverge from C#/TS/Python. `T[]`
(`Array.type: "array"`, `:189-190`) shares the identical root cause. Two std mappings already reassign
`$this` (`clear`, `removeAll`) — proof the current representation is value-typed.

**Design — `\ArrayObject`, a genuine PHP reference type, for both `List<T>` and `T[]`:** closes the whole
class (params + aliasing + fields) uniformly. The surface is smaller than it looks because ArrayObject
implements ArrayAccess + Countable + IteratorAggregate — index get/set, `$this[] = x` append,
`count($this)`, and `foreach` are **unchanged**. Rewrites (~5 entries + construction): `init`/`ListLit` →
`new \ArrayObject([...])`; `clear` → `$this->exchangeArray([])`; `removeAll` →
`exchangeArray(array_values(array_filter($this->getArrayCopy(), …)))`; `removeAt` →
copy/`array_splice`/`exchangeArray`. Native-array escapes go through `getArrayCopy()` (today only List's
own `array_filter`/`array_splice` sites).

**Rejected alternatives.** *(a) `&$xs` by-ref params:* fixes only the parameter symptom while aliasing
stays silently broken — worse than the status quo because it *looks* fixed; PHP also fatals on passing a
non-variable by reference (`f([1,2,3])`, `f(g())`), which would need a temp-spill lowering; and `foreach`
by-ref is its own footgun. *(c) refuse list-param mutation in the checker:* bans a §3.A-supported pattern
on all four targets to satisfy one — too blunt, and still leaves aliasing.

### 2.5 · #29 — interfaces: close the gaps in an already-shipped feature

**Already working at HEAD (verified by running the issue's MCTS shape):** `interface` declarations
(`parser.cpp:340`, `ast.hpp:239`, `ir.hpp:483`), `class X : IFoo` with comma-lists (multiple interfaces),
base-class+interface mix (`decl.extBase`/`decl.ifaceBases` split), interface inheritance, generic
interfaces (sample 04's `Comparable<T>` is in the permanent gate), interface-typed
params/fields/returns/locals, nullable interfaces, and virtual dispatch — correct on **C#, TypeScript, and
PHP**. The issue's minimum-viable ask is, on three targets, done. The gaps:

1. **Python miscompiles** (the §3.B cardinal sin): the plugin declares `"interfaces": "emulated"`, emits
   **no** interface declaration, yet `ClassDecl` emits all bases → `class PgPolicyValueNet(IPgNet):` with
   `IPgNet` undefined → `NameError` at import time.
2. **No implements-conformance check:** a class declaring `: IShape` with the method missing or
   wrong-signatured gets zero diagnostics; the failure surfaces later in the *target* compiler.
3. **No nominal assignability:** a non-implementing class assigns to an interface-typed slot silently —
   `checkConvert` (`sema.cpp:772-802`) only compares scalars; named user types never mismatch.
4. **Adjacent, newly found — `match` typed-pattern narrowing miscompiles on all four targets:**
   `lower.cpp:301` silently drops the pattern's type annotation, so the first arm unconditionally wins
   (C# output doesn't even compile). The `is` operator is in `grammar.ebnf` but not in the lexer/parser.
5. **Interface bodies over-accept:** `parseInterface` reuses `parseMemberBlock`, so fields, properties,
   bodied methods, and statics are accepted and **silently dropped** at lowering (`lower.cpp:184-185`).

**Design decisions:**
- **D1 · Python mapping = ABC, not Protocol.** `.pg` interfaces are *nominal* (explicit implements clause,
  checker-enforced); `Protocol` is structural and its `@runtime_checkable` `isinstance` ignores
  signatures. ABC keeps the already-emitted class base list (smallest diff), gives real `isinstance` for
  future narrowing. Emission: `InterfaceDecl` rule → `class I(abc.ABC):` + `@abc.abstractmethod` stubs +
  the existing `require` preamble primitive for `import abc`. Pure plugin-JSON change; flipping
  `"emulated"` to a real rule puts it under the anti-silent-drop coverage contract.
- **D2 · Multiple interfaces per class: in** (already works — free).
- **D3 · Interface inheritance: in** (parse+emit already work; the checker walks transitive bases via the
  existing `TypeInfo.bases`).
- **D4 · Generic interfaces:** declaring/implementing them stays allowed (sample 04 depends on it); the
  conformance check substitutes base type-args via the existing `substGeneric` (`sema.cpp:829`).
- **D5 · `is`/`as` narrowing: out**, per the issue — **but** the typed-pattern miscompile (gap 4) must
  close *now*: sema refuses a match binding annotated with a class/record/interface type other than the
  scrutinee's own, with a clear diagnostic. Real type-test patterns become a follow-up issue
  *(maintainer decision, 2026-07-18: confirmed as a separate follow-up)*.
- **D6 · `override` follows C# conventions** *(maintainer decision, 2026-07-18)*: `override` marks only
  an override of a **virtual/abstract base-class member** — implementing an interface method takes **no**
  `override`, and writing one there is an error. SPEC's old "override required when implementing an
  interface method" rule (a Kotlin-ism sample 04 obeyed) is **removed**; SPEC and sample 04 are updated
  in this PR. Clean cutover per the no-backward-compat policy.
- **D7 · Interface bodies restricted in sema:** bodiless, non-static `fn` signatures only; fields /
  properties / bodies / const → diagnostics (holds the issue's v1 scope line, kills silent drop #5).

### 2.6 · README — demo-repo link

Add <https://github.com/PieterjanDeClippel/qrcode-demo> to `README.md` — a real-world consumer (QR
encoder single-sourced to all four targets) and the dogfood project that surfaced #33–#36. While there,
refresh the stale "PHP is a partial target" call-out: the plugin's refusal list has shrunk since P26
(the conformance runner now gates only operator overloading + async), and this PR shrinks it further.

---

## 3. Scope-contract alignment (PRD §3)

- **§3.B "clear diagnostic — never a miscompile"** is the heart of the batch: #33 (silent constant
  corruption), #34 TS/Python/PHP (silent 0/dropped cast → new diagnostic + portable accessor), #29 gap 1
  (Python `NameError`), gap 2/3 (errors surfacing in the wrong compiler), gap 4 (first-arm-wins `match`),
  and #36 (silent cross-target divergence) all move from silent-wrong to correct-or-refused.
- **§3.A Supported.** Interfaces, collections, strings, enums/ADTs stay in the supported core; nothing is
  removed. `codePointAt` is a std addition mapping natively on every target. The only *narrowings* are
  `(i32)char` (now a diagnostic with a named alternative) and typed-pattern annotations (now refused
  instead of miscompiled) — both previously produced wrong code on ≥3 targets.
- **§3.C Faithful-by-default.** PHP list reference semantics joins the *faithful* column (no relaxation
  entry needed — it now matches the other targets). No new relaxations introduced.
- **§3.D determinism.** Untouched — all new conformance programs print ints/strings only.

## 4. Acceptance criteria

1. **#33:** `0xff`→255, `0x11d`→285, `0xf32` stays `0xf32`, `0xFF` unchanged, `0xffi64` still strips its
   integer suffix — identical on all four targets; unit asserts on token text + lowered IR.
2. **#34:** `"A".codePointAt(0)` prints 65 on all four targets (conformance program); `(i32)'A'` is a
   compile error naming `codePointAt`; `(i32)"str"` stays diagnosed (regression).
3. **#35:** a module `let` table + `const` read from a free fn, a class method, and a property getter
   prints identically on all four targets (`module_globals.pg`); emitted PHP methods/getters carry
   `global $X;`; a shadowing param/local suppresses it; free-fn path regression-guarded.
4. **#36:** mutation through a list param, alias mutation, `T[]` element-write through a param, and
   list-in-class-field all print identically on all four targets (four conformance programs); emitted PHP
   uses `new \ArrayObject(...)` / `exchangeArray`.
5. **#29:** the issue's two-nets-one-MCTS shape runs identically on all four targets
   (`interfaces_mcts.pg`) — including Python; a class missing an interface method / wrong signature /
   missing `override` (per D6) / non-implementing assignment / field-in-interface each gets a clear
   diagnostic (unit `rejects`); sample 04 (`Comparable<T>` + record + override) stays green.
6. **Docs:** README gains the demo-repo link + refreshed PHP call-out; SPEC.md documents conformance
   checking, `override`, the Python-ABC mapping, `codePointAt`, and the `(i32)char` refusal; PLAN.md
   gains the P31 milestone log.
7. `scripts/build-and-test.ps1` fully green (incl. the C#↔PHP leg), **run once at the end**; POSIX leg
   compiles (lexer/lower touch platform-neutral code only, but the gate's ubuntu `ci.yml` check is the
   pre-merge floor).

## 5. Out of scope

- **`is`/`as` type-test narrowing** (D5) — follow-up issue; this PR only *refuses* the currently
  miscompiling typed pattern.
- **`Ty::Char` in the scalar lattice** — the name-based guard suffices for the two cast sites; a lattice
  extension is recorded as the principled follow-up if char arithmetic is ever wanted.
- **PHP `charAt`/`codePoints` multibyte semantics** — pre-existing byte-based behavior left alone;
  `codePointAt` is multibyte-correct from day one.
- **Default interface methods, interface properties** — per the issue's own v1 scope.
- **Boxing lists across native-FFI boundaries beyond the std's own sites** — watched, not built.

## 6. Versioning & PR mechanics

Lockstep + tag-driven (P24): sources stay `0.0.0-dev`; the **PR carries `release:minor`** (new std
surface `codePointAt`, Python interface emission, checker tightening). One branch, one PR to `master`
folding design + all slices (maintainer's single-PR preference); on merge, comment on and close all five
issues, each pointing at its slice + regression tests. No compat shims — clean cutover.

---

*Implementation plan: see [PLAN.md](./PLAN.md).*
