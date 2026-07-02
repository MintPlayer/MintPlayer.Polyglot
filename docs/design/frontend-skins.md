# Alternative input syntaxes ("skins") — design & decision record

*2026-07-02 — from a 4-agent investigation (seam architecture, C#/TS syntax mapping, data-driven
front-ends, prior art & risk). PRD summary: §4.12. Roadmap: PLAN §P20 (gated).*

**The user need:** a developer who wants Polyglot's multi-target output but doesn't want the `.pg`
syntax "subjected onto him" — let them author in a C#- or TypeScript-flavored syntax instead. This is
the *syntax-skin* idea (Reason/ReScript over the OCaml AST): a different surface over the **same**
language semantics — the §3.A feature set, nothing more. It is explicitly NOT "compile arbitrary C#/TS."

**Decision in one line:** the *seam* is cheap and worth building (P20 slice 1); the *cheap satisfiers*
ship first (Rosetta docs + the already-shipped P17 live preview); the **TypeScript skin is refused
permanently** (it misleads exactly where miscompiles live); the **C# skin is gated, not scheduled**
(post-P19, frozen grammar, observed demand) — and if it ever ships, it is a compiled-in front-end,
never a data plugin.

---

## 1. The seam — where an alternative front-end plugs in (seam agent)

**The seam already exists de facto: the unchecked AST (`CompilationUnit`).** Everything downstream of
`parse()` — sema, lower, backends, capability gating — consumes only the AST; no pass reaches back into
tokens or trivia. The AST is already syntax-neutral where it counts: node kinds are semantic
(`ExprKind`/`StmtKind`/`MemberKind`), `Member::modifiers` is a canonical closed string vocabulary the
parser maps its keywords into, `TypeRef` is structural. The only syntax residue carried through is
`SourcePos` (line/col/fileId) — a second front-end must stamp accurate positions into *its* source text,
and the whole LSP (go-to-def/rename/hover/tokens) then transfers for free because `SemanticModel` is
built from AST spans inside sema.

**The seam must be the AST, not the IR.** Emitting IR directly would bypass resolution, typing,
exhaustiveness, overload mangling, and capability gating — a skin must never reimplement those. A
front-end produces an unchecked `CompilationUnit` with accurate positions + the canonical modifier
vocabulary, and does not set sema-owned fields.

**Proposed shape — mirror the `BackendHandle` pattern:**

```
class Frontend {
  virtual CompilationUnit parse(const std::string& source, DiagnosticBag&, int fileId) const = 0;
  virtual std::string print(const CompilationUnit&) const = 0;   // fmt/convert; optional at first
  virtual const std::vector<std::string>& keywords() const = 0;  // LSP completion
};
```

- The existing `lex`+`parse`+`printSource` wrap as the default **`PgFrontend`**; `FrontendHandle` +
  `findFrontend(name)` clone `BackendHandle`/`findTarget`.
- `compile`/`analyze`/`format` gain a `FrontendHandle` param (default pg); ~5 call sites re-route.
- **Dispatch by file extension, in the CLI** (Core stays IO-free): `.pg` → pg, `.pgcs` → C# skin.
  Never `.cs` — the distinct extension is a scope-line defense (see §4).
- **Mixed-syntax projects:** `ResolvedModule` gains a front-end tag (the CLI resolver derives it from
  the resolved file's extension); the linker dispatches per module. **std stays `.pg`** regardless of
  the entry file's syntax.
- Estimated seam cost: **~1–2 days of plumbing, no AST/sema/lower/backend change.** The dominant cost
  is each new front-end (~parser-sized, ~1,100 lines for the `.pg` reference), not the seam.

**`fmt` catch:** `printSource` is a `.pg` printer — run on a skin file it would silently rewrite it to
`.pg`. Each skin wanting `fmt` needs its own printer (≈ doubles the skin's cost); a parse-only skin
(build/check/LSP-nav) ships without one.

**Convert is AST-level, not backend-level.** Backends emit from post-lower IR — desugared,
overload-mangled, bindings inlined; that output is one-way target code, not reparseable skin source. So
`polyglot convert` = parse with front-end A → print with front-end B. Corollary worth noticing: **a
one-way C#-skin → `.pg` migration aid needs only the skin parser + the existing `.pg` printer** — no
new printer. Comment preservation is a separate cross-cutting effort (comments are dropped at the lexer
today; `fmt` already loses them), so any convert v1 is "semantics-preserving, comments-dropped."

## 2. Skin fidelity — how well C#/TS surfaces map onto `.pg` (skin agent)

### TypeScript skin — refused (net-negative, permanently)

The TS surface **misleads precisely where `.pg` is most careful**, i.e. where miscompiles are dangerous:

| trap | TS reading | `.pg` semantics |
|---|---|---|
| integer widths | one `number` — widths **inexpressible** | the whole i8…u64 faithfulness model keys off declared width |
| casts | `x as T` is **erased**, a no-op | `(T)x` truncates/wraps at runtime |
| `let` | mutable | **immutable** — semantics inverted |
| `for..in` | iterates **keys** | iterates values (TS `of`) |
| `A \| B` unions | structural, open | nominal, closed, exhaustiveness-checked |

Its genuine 1:1 wins (imports — which `.pg` copied from TS verbatim — arrows, generics, `function*`,
TS 5.2 `using`) don't offset traps that are *inverted* rather than merely missing. Uncanny valley;
refused with the same finality as a §3.B feature.

### C# skin — defensible, "C#-flavored," gated

C# maps a clear majority at or near 1:1, and wins decisively exactly where TS fails: **integer widths
are a clean name bijection** (`int↔i32`, `long↔i64`, `byte↔u8`, …) and **`(int)x` casts share
truncate/wrap semantics**. Records + `with` + structural `==`, null handling (`T?`/`?.`/`??`/`!`),
`switch` expressions ↔ `match`, multi-arg indexers, tuples, properties, lambdas, `where T :` bounds
(C# even has real `INumber<T>`) all land cleanly.

But the skin must still **invent** syntax where C# has none — discriminated `union` (the biggest gap;
the closed-record-hierarchy encoding is too lossy and can't enforce exhaustiveness), TS-style selective
imports, range `for i in 0..n` — and **reshape** operators (instance 1-arg vs static 2-arg),
extensions, ctor spelling (`init` vs class-name), and async (the author writes unwrapped `T`, not
`Task<T>`). So the honest name is **"C#-flavored Polyglot,"** not "C# input." That's still plausibly
pleasant — unlike the TS case, the gaps are *absences* (no false familiarity), not inversions.

## 3. Front-ends are compiled-in code, not data plugins (grammar agent)

**Grammar-as-data is rejected** (recorded, with rationale, so it isn't relitigated casually):

- A backend emits a *total, deterministic* function over an already-valid tree — that's why the P19
  JSON DSL works and stays RCE-safe. A front-end runs a *partial, ambiguous, error-prone* function over
  arbitrary text: disambiguation, error recovery, diagnostics. The parsing equivalent of emit templates
  is a declarative AST-construction action language — a second interpreter, *inverse to and harder
  than* the emit DSL, that can't stay non-Turing-complete while expressing real disambiguation.
- The current parser is ~90% procedural by nature: unbounded-lookahead disambiguation
  (`genericCallAhead`, `isLambdaAhead`, `isCastAhead`), angle-bracket splitting, contextual keywords,
  interpolation lexing with a brace-depth stack, and **panic-mode error recovery** — the thing the LSP's
  live diagnostics depend on and the least declarative aspect of parsing (tree-sitter exists to solve
  it, at the cost of a runtime dep the zero-dep charter forbids). PEG-style data grammars also regress
  diagnostics to "no alternative matched" — against a stated core value.
- Precedent is unanimous: no production syntax skin ships a runtime-interpreted code-free grammar.
  Reason/ReScript hand-write their parser + printer; Civet's PEG has embedded Turing-complete JS
  actions, generated at build time.
- **The asymmetry with P19 is principled:** targets are open-ended (the economic case for data plugins
  is third parties adding languages with zero Core PRs); front-ends are few (1–3) and stable — a
  one-time cost. And the three things hand-written parsers are best at (disambiguation, recovery,
  crisp diagnostics) are the three this project prizes.
- **Packaging can still be symmetric:** `polyglot-plugin.json` may *declare* a front-end by naming a
  Core-registered parser (`"frontend": {"parserId": "csharp-skin"}`) — symmetric manifest, asymmetric
  implementation. Surface *tables* inside a compiled-in parser (keyword map — already data — operator
  spellings, type-name aliases) are fine and cheap; surface *structure* is not data.

## 4. Prior art, risk, and the strategic call (risk agent)

**Prior art reads as a warning.** Reason — the flagship "familiar syntax over a different core" — ended
in a three-way governance fork (Reason/ReScript/Melange) with the syntax layer (`refmt` round-tripping)
a large permanent cost; community verdict: it bifurcated the ecosystem. CoffeeScript faded when the base
language absorbed its value. Scala 3's optional braces show the two-syntax tax bites even within one
language. Meanwhile Kotlin/Swift adopted fine with brand-new syntax — **syntax familiarity is not the
adoption lever; tooling + value are.** The "JS-lookalike attracts JS devs" thesis mostly failed: the
real learning curve was semantic, and familiar braces did nothing to soften it.

**Risk register (top three):**
- **Maintenance multiplier (high):** each skin multiplies grammar, formatter, LSP behaviors, TextMate
  grammar, docs/samples, and error messages — forever, for every future language feature — and the cost
  lands on the front-end, exactly the layer P19 *cannot* make data-driven. On a one-person project
  there is no team to absorb it.
- **Expectation trap / scope pressure (high):** C#-looking code invites LINQ, `Task.Run`, reflection,
  `decimal` — the §3.B refused list — and each refusal lands harder than in native `.pg`, where the
  author never assumed the host BCL existed. The native syntax is *honest*; a skin deletes the signal
  that says "different language, different rules." This is the single most effective delivery vehicle
  for the scope-creep that killed JSIL/SharpKit/Bridge.NET.
- **Fragmentation (medium today, high with users):** "which syntax are the tutorials in?"

**The need is already ~80% met.** `.pg` is deliberately TS-flavored (`import {} from`, `x: i32`, `=>`,
`${}`), and P17's live generated-output preview — already shipped — is the *actual* reassurance a
C#/TS developer wants: type `.pg`, watch idiomatic C#/TS/Python appear beside it. The remaining gap
closes with a near-free **Rosetta cheat-sheet** ("Polyglot for C# developers" / "…for TypeScript
developers", construct-by-construct), which must explicitly flag the `let` false-friend (immutable in
`.pg`, mutable in TS).

## 5. The decision — staged and gated

1. **Now, near-free:** Rosetta cheat-sheet docs + a docs note leaning on the P17 preview. No code.
2. **P20 slice 1 (cheap, anytime after P19):** the `Frontend` seam refactor — pure plumbing, no
   behavior change, keeps the door open at negligible cost.
3. **Gated — only on *observed* demand (real external users asking, not speculation):** the one-way
   **`polyglot convert`** import aid (C# *subset* → `.pg`; loud failures on anything unsupported, no
   round-trip promise, comments dropped, framed as migration-only). Note it's the C# skin *parser*
   reused with the existing `.pg` printer — so it is also the cheapest honest test of skin demand.
4. **Gated harder — C# authoring skin (`.pgcs`):** only if convert proves sustained demand AND P19 is
   done AND the `.pg` grammar is frozen. Compiled-in parser, distinct extension, dialect banner in docs
   + LSP, skin-scoped refusal diagnostics ("this looks like C#, but Polyglot's C# skin doesn't include
   LINQ — use `for`/`yield`"). Parse-only first (no printer → no fmt) is acceptable.
5. **Never: the TypeScript skin.** Inverted semantics on widths/casts/`let`/`for..in`/unions — it
   misleads exactly where §3's faithfulness machinery lives.

**Contract language — PRD §3.F** makes this checkable (single deliberately-distinct authoring syntax;
reading aids + one-way convert supported; TS skin refused; C# skin admitted only through the gate).

## 6. What this costs if the gate ever opens

| item | size | notes |
|---|---|---|
| Frontend seam | ~1–2 days | plumbing only; no AST change |
| C# skin parser | ≈ `.pg` parser (~1.1k lines) | hand-written RD over the shared AST |
| `polyglot convert` (skin→pg) | small once parser exists | reuses `.pg` printer |
| skin printer (fmt, pg→skin) | ≈ another parser | optional; parse-only skin ships without |
| TextMate grammar + LSP keywords | small | `Frontend::keywords()`; grammar per skin |
| docs/samples/error messages | permanent tax | the irreducible multiplier |
