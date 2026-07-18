# Open-issue batch (#29, #33–#36) + README — implementation plan

Companion to [PRD.md](./PRD.md). One branch, **one PR**, slices ordered cheap→big. Discipline (project
convention): **byte-gate** every slice — diff emitted C#/TS/Python/PHP for the existing conformance +
samples corpus; the only allowed output deltas are the ones the slice intends. **Do NOT run the full gate
per slice** — implement everything, then run `scripts/build-and-test.ps1` (or `/build-and-test`) **once**
at the end (the cheap unit-test exe is the mid-flight ceiling). Build: VS 18 Insiders MSBuild (v145).

Regression-test convention (per the infra survey): diagnostics + golden substrings as `issue#N:`-named
`check`/`rejects`/`refuses` blocks in `tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp`; runtime
cross-target behavior as `tests/conformance/programs/*.pg` (auto-discovered by all three differential
runners — no manifest; keep output deterministic: ints/strings only).

Sign-offs resolved (2026-07-18): **D6** — `override` follows **C# conventions** (only on
virtual/abstract base-class overrides; an interface implementation takes no `override`, a stray one is
an error; SPEC + sample 04 updated). **D5** — `is`/type-test narrowing is a separate follow-up issue.

---

## Slice 0 — README: demo-repo link + stale-PHP refresh

- `README.md`: add <https://github.com/PieterjanDeClippel/qrcode-demo> — a real-world consumer (QR
  encoder emitting all four targets; the dogfood that surfaced #33–#36) — as a "See it in action" bullet
  near the top links; refresh the "PHP is a partial target" call-out (refusal list has shrunk since P26;
  this PR shrinks it further — final wording in Slice 6 once the PHP deltas are in).

**Gate:** docs-only.

## Slice 1 — #33: radix-aware number lexing + suffix stripping

- `src/MintPlayer.Polyglot.Core/src/lexer.cpp` (~165-195): real radix branch — on `0x`/`0X` (and
  `0b`/`0o`) scan base-appropriate digits, then accept **only integer** width suffixes (`i8`…`u64`);
  always `IntLit`, never a float suffix.
- `src/MintPlayer.Polyglot.Core/src/lower.cpp` (:20-27, call site :459): `stripNumericSuffix` never
  strips `f`/`d`/`f32`/`f64` from a `0x`/`0b`/`0o`-prefixed literal.
- **Regression (`tests_main.cpp`, lexer blocks ~198-221 + `callIr`):** `issue#33:` — `0xff`→255,
  `0x11d`→285, `0xf32` survives intact (the unreported catastrophe), `0xFF` unchanged, `0xffi64` still
  strips `i64`; raw-token assert `.text == "0xff"`.

**Gate:** unit exe; byte-gate = zero deltas (no corpus program uses lowercase-suffixed hex).
~25 lines production.

## Slice 2 — #34: `codePointAt` + `(i32)char` diagnostic

- `src/MintPlayer.Polyglot.Core/src/compiler.cpp` (`STD_STRINGS`, ~159-174): add
  `extension fn string.codePointAt(index: i32): i32 {}`.
- One arm per plugin beside `string.charAt`: csharp `((int)$this[$0])` · typescript
  `$this.codePointAt($0)!` · python `ord($this[$0])` · php `mb_ord(mb_substr($this, $0, 1))`.
- `src/MintPlayer.Polyglot.Core/src/sema.cpp` (:1080-1087 `checkCast`, :1318-1320 call-cast): name-based
  guard — casting `char` to a numeric type errors: *"cannot cast char to a numeric type — use
  s.codePointAt(i) for the ordinal"*.
- **Regression:** conformance `programs/codepoints.pg` (`print("A".codePointAt(0))` etc. → 65…);
  `issue#34:` unit asserts — per-target golden substrings (`(int)`, `.codePointAt(`, `ord(`, `mb_ord(`),
  `rejects` for `(i32)'A'` naming char, regression that `(i32)"str"` stays diagnosed.

**Gate:** unit exe; byte-gate = zero deltas (new surface only). Small.

## Slice 3 — #35: `global` prologue for PHP methods + property getters

- `src/MintPlayer.Polyglot.Core/include/.../ir.hpp`: `ir::Method` gains
  `std::vector<std::string> globalRefs;` (mirror `ir::Function`, :412).
- `src/MintPlayer.Polyglot.Core/src/lower.cpp`: hoist the module-global name-set to a `Lowerer` member
  (built in the ctor from `unit.values` — the record/class loops run before the current local set at
  :208-209; swap the :224/:242 uses); `method()` (:430) sets
  `globalRefs = scanGlobalRefs(…, m.body, m.exprBody.get())` — **exprBody included so property getters
  are covered**.
- `src/MintPlayer.Polyglot.Core/src/emitter_base.cpp` (`MethodDeclCtx::get`, ~706): expose
  `decl.globalRefs.count` / `.<i>` (mirror `FnDeclCtx` :947-950).
- `plugins/php/polyglot-plugin.json`: factor the `global $…;` prologue out of `phpFnBody` (:1863-1930)
  into a shared macro; invoke from `MethodDecl` (:2027-2062). C#/TS/Python ignore the field.
- **Regression:** conformance `programs/module_globals.pg` (module `let TABLE: i32[]` + `const`, read
  from a free fn, a record method, and a property getter — fails on PHP before, passes after);
  `issue#35:` unit asserts — method + getter emit `global $X;`, shadowing param/local suppresses it,
  free-fn path unchanged, closure-inside-method covered.

**Gate:** unit exe; byte-gate = PHP methods/getters gaining `global` lines only. Small.

## Slice 4 — #36: `\ArrayObject` reference semantics for PHP `List<T>` + `T[]`

- `plugins/php/polyglot-plugin.json` (:159-166 List, :189-190 Array, :555 ListLit):
  `type` → `\ArrayObject`; `init`/`ListLit` → `new \ArrayObject([...])`; `clear` →
  `$this->exchangeArray([])`; `removeAll` →
  `$this->exchangeArray(array_values(array_filter($this->getArrayCopy(), …)))`; `removeAt` →
  copy + `array_splice` + `exchangeArray`. Index get/set, `$this[] =` append, `count($this)`, `foreach`
  are unchanged (ArrayAccess/Countable/IteratorAggregate). Native-array escapes via `getArrayCopy()`
  (only List's own filter/splice sites today). `T[]` gets the same treatment — **don't fix List and
  leave arrays silently broken**.
- Core touch only if `T[]`-literal boxing needs an emit-side branch (`type.name == "Array"` is already
  visible at `emitter_base.cpp:157`).
- **Regression:** four conformance programs — `list_param_mutation.pg` (the reported bug),
  `list_alias.pg` (the class `&`-params would have missed), `array_param_mutation.pg` (`T[]`
  element-write through a param), `list_in_class_field.pg` (must-stay-working guard); extend
  `programs/collections.pg`'s read-only param to also mutate. `issue#36:` unit asserts on
  `new \ArrayObject(` construction + `exchangeArray` rewrites.

**Gate:** unit exe; byte-gate = PHP-only deltas confined to list/array construction + the rewritten std
arms. Second-biggest slice.

## Slice 5 — #29: checker honesty (conformance, assignability, override, bodies, typed patterns)

All in `src/MintPlayer.Polyglot.Core/src/sema.cpp` (~+300 lines):
- **(a) Implements-conformance pass** (after the type tables): for classes **and records** (both carry
  bases, `ast.hpp:223`), walk the transitive interface closure (`TypeInfo.bases`, :180), substitute
  generic base args via `substGeneric` (:829), and require a matching method per signature
  (overload-aware via `sameParamList`, return-type compared). Diagnostics: missing method, wrong
  signature, unknown interface in the implements clause.
- **(b) `override` per D6 (C# conventions)** — `override` is valid only on a method overriding a
  virtual/abstract **base-class** member; an interface implementation carries no `override` and a stray
  one is an error. Update SPEC + sample 04 (which used override on interface impls) accordingly.
- **(c) Interface-body restrictions per D7:** bodiless non-static `fn` signatures only; fields /
  properties / bodied methods / const / init → diagnostics (closes the accept-then-silently-drop hole,
  `lower.cpp:184-185`).
- **(d) Nominal assignability in `checkConvert`** (:772-802): when both sides are fully-known named user
  types, require identity or class/record-implements-interface (transitive). Conservative guards — both
  in `types_`, complete, not active generic params, not Option/List/Array/scalars — to avoid false
  positives on the existing corpus.
- **(e) Typed-pattern refusal per D5** (~20 lines): a `match` binding annotated with a class / record /
  interface type other than the scrutinee's own static type is refused with a clear diagnostic (closes
  the first-arm-always-wins miscompile at `lower.cpp:301`); file the `is`/type-test-pattern follow-up
  issue on merge.
- **Regression:** `issue#29:` unit `rejects` — missing method, wrong signature, missing/stray override,
  non-implementing assignment, field-in-interface, annotated foreign-type pattern; **accepts** — the
  MCTS shape, multi-interface class, interface inheritance, sample 04's `Comparable<T>` record
  (the canary — it sits in the permanent gate).

**Risk (top of the batch):** tightening the checker may redden existing `.pg` (std modules, samples,
FruitCake dogfood). Mitigation: the conservative guards above; anything still red gets *fixed forward*
per the no-backward-compat policy and recorded in the P31 log.

## Slice 6 — #29: Python interface emission (ABC)

- `plugins/python/polyglot-plugin.json`: real `InterfaceDecl` rule — `class I(abc.ABC):` +
  `@abc.abstractmethod` stubs — using the existing `require` preamble primitive (`emitter_base.cpp:395`)
  for `import abc`; flip `"interfaces": "emulated"` to supported (the anti-silent-drop coverage contract
  then enforces the rule's existence); `ClassDecl` keeps emitting bases (now defined). Pure plugin-JSON.
- **Regression:** conformance `programs/interfaces_mcts.pg` — the issue's shape (two nets, one
  interface, MCTS dispatch through an interface-typed param) — auto-runs on the C#↔TS, C#↔Python, and
  C#↔PHP legs; `issue#29:` golden asserts on the emitted `abc.ABC` class + abstractmethod stubs.

**Gate:** unit exe; byte-gate = Python outputs gaining interface classes + `import abc` only.

## Slice 7 — Docs + the single full gate + PR

- `docs/lang/SPEC.md`: conformance checking + `override` rule (per D6 outcome) + Python-ABC mapping +
  `codePointAt` + the `(i32)char` refusal + PHP list reference-semantics note; `docs/lang/grammar.ebnf`
  untouched (no new syntax) except removing the phantom `is` operator or marking it reserved.
- `README.md`: finalize the PHP-partial call-out wording (post-#35/#36/#29 the refusal list shrinks).
- `docs/prd/PLAN.md`: append `## P31 — open-issue batch` milestone log (house style: root causes,
  what shifted vs the issues' text, +7 conformance programs).
- Root `CLAUDE.md`: one-line status touch (P24 D6 discipline).
- **The one full gate:** `pwsh scripts/build-and-test.ps1` — build → unit → fidelity/watch/registry →
  differential C#/TS + C#/Python + C#/PHP → samples/nullable/library. All slices verified here, once.
  No platform-forked code is touched (lexer/lower/sema/plugins are platform-neutral), so the PR's
  ubuntu `ci.yml` check is the POSIX floor.
- **Close-out:** single PR to `master`, label **`release:minor`**; body maps each issue → slice →
  regression tests; on merge comment/close #29 (noting C#/TS/PHP already worked at 0.3.2 + what was
  added), #33, #34 (noting the two stale bullets), #35 (noting the free-fn path was already fixed;
  methods/getters were the live gap), #36 (noting aliasing was also covered); file the `is`/type-test
  follow-up issue.

---

## Summary of touch-points

| Slice | Issue | Area | Files | Size |
|---|---|---|---|---|
| 0 | README | docs | `README.md` | trivial |
| 1 | #33 | lexer + lowering | `lexer.cpp`, `lower.cpp`, `tests_main.cpp` | ~25 loc |
| 2 | #34 | std + sema + 4 plugins | `compiler.cpp`, `sema.cpp`, `plugins/*/polyglot-plugin.json`, tests, 1 program | small |
| 3 | #35 | IR + lowering + emitter + PHP plugin | `ir.hpp`, `lower.cpp`, `emitter_base.cpp`, `plugins/php/…`, tests, 1 program | small |
| 4 | #36 | PHP plugin (+Core touch?) | `plugins/php/…`, tests, 4 programs | medium |
| 5 | #29 | checker | `sema.cpp` (+~300), tests | large |
| 6 | #29 | Python plugin | `plugins/python/…`, tests, 1 program | ~120 loc |
| 7 | all | docs + gate + PR | `SPEC.md`, `README.md`, `PLAN.md`, `CLAUDE.md` | docs |

Estimated net: ~1,200–1,500 lines across production + tests; one focused implementation pass + the
single ~15-minute gate.
