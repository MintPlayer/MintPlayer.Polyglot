# Design note — Plugins, targets & environments

> **Status:** Design note v0.2 · 2026-06-28 · refines PRD §4.4 and §4.3. Captures decisions taken during
> P1 review. Not a milestone deliverable — the plugin system proper lands at P9/P10 (§7). This note exists
> so every milestone before it is built *toward* this architecture instead of away from it.
>
> **v0.2 change:** plugins are now **declarative data interpreted by the core** (not loadable executable
> code), downloaded + versioned via a workspace config; **target backends are themselves declarative
> plugins**; and there are **two plugin tiers** (declarative-downloaded vs. full-power-local). This
> supersedes v0.1's "full power from the start."

---

## 1. The premise: the core is a pure translator + a declarative emit engine

The core compiler knows exactly three things and nothing else:

1. **The Polyglot language** — the §3.A surface, lexed/parsed/type-checked and lowered to the one typed IR.
2. **A minimal portable std** — the types guaranteed identical everywhere (collections, string, math,
   iterators), written in `.pg`.
3. **A declarative emit engine** — it walks the IR and produces target source by interpreting a
   **declarative backend spec** (§4). It has no built-in, hardcoded knowledge of C#, TypeScript, Python, or
   any platform API; it only knows how to *interpret a spec*.

Everything beyond this — every target language's pretty-printer, every platform/SDK/environment API — is a
**plugin**: data the engine interprets. The core ships *bundled* specs for C# and TS and the portable std,
but those are plugins in the same format a third party would write.

This is the scope line at its sharpest: the core's platform surface is **zero by construction**, and even
its *target* surface is data, not code. It cannot drown in what it does not contain.

## 2. The three mechanisms a plugin provides

| Mechanism | What it does | Analogue |
|---|---|---|
| **Binding** | Declares the *shape* of an existing target API and a template to emit calls to it (`window`, `WinForms`, `fs.readFile`). Target-scoped. | TS `.d.ts`, Haxe extern |
| **Replacement** | Maps a *portable* API to **different** idiomatic code per target (`List.map` → `.Select(…)` / `.map(…)`). | Fable Replacements |
| **Capability** (`expect`/`actual`) | Portable code names an abstract need (`nowMillis()`); each target's plugin supplies the concrete `actual`. | Kotlin Multiplatform |

A **backend** (§4) is the largest plugin of all: the complete IR→source mapping for one target. Plugins
expose their additions through a **public API** that `.pg` code imports and calls like any other module.

## 3. Two plugin tiers (this supersedes "full power from the start")

The earlier "full power from the start" decision is **refined** in light of the safe-download goal:

- **Downloaded plugins are declarative data only.** No arbitrary code runs anywhere in the toolchain when a
  plugin is fetched or interpreted. This is what makes "read `pgconfig.json`, download, transpile" safe (§6)
  and what lets a self-contained C++ core interpret them with **no host runtime dependency** (§ PRD 4.3).
- **Local plugins may be full-power.** A developer can author a *local, trusted* plugin for advanced cases
  the declarative DSL cannot express. Because it is local and trusted, the download-safety concern does not
  apply; how full-power local plugins attach to a C++ core (native module vs. a local scripting hook) is an
  open mechanism question (§8), deferrable because nothing forces it early.

"Full power" was really about *emission expressiveness*. A sufficiently rich declarative DSL (§4) delivers
that for the common case; the local tier is the pressure valve for the rest.

## 4. Backends are declarative plugins — and the declarative DSL is the hard part

A target language (C#, TS, Python, …) is a **backend spec**: a declarative plugin the emit engine
interprets. What a spec must contain:

- **A rule/template per IR node kind** (with slots for child emissions), parameterized by *context* —
  expression-vs-statement position, operator precedence (for correct parenthesization), etc.
- **The std-type mapping** (`List<T>` → `System.Collections.Generic.List<T>` / `Array<T>`; `i64` → `long`
  / `bigint`).
- **Operator/keyword tables**, **naming rules** (overload mangling, casing conventions), **import/preamble
  management**, and the **build-project scaffold** + the points where build dependencies (§ below) are
  injected.
- **A capability set** — the named §3.E feature flags this backend can emit (`extensionMethods`,
  `operatorOverloading`, `properties`, `iterators`, `patternMatching`, …). The core **intersects** these
  across all targets configured in `pgconfig.json`: a §3.A feature is usable iff *every* configured backend
  declares it, else a compile-time refusal names the capability + the lacking target. The bundled C# and TS
  specs both declare the full §3.A set (so the intersection is everything and nothing is gated until a third
  backend with a smaller set arrives). **Do not confuse this with the `expect`/`actual` "Capability"
  mechanism in §2** — that fills a *platform-API* need (time/IO) per target; *this* is about whether a
  *core language feature* can be expressed idiomatically at all on a target. (Cross-SDK reality: extension
  methods keep `x.method()` on C#/Kotlin/Swift/Dart/Rust/Ruby but not Java/Go/C++/PHP; see PRD §3.E.)

> **The central challenge (be honest about it):** emitting *idiomatic, readable* code — the PRD §2 bar — is
> full of context-sensitive decisions (precedence, `for` vs `foreach`, switch-expression vs -statement,
> using/import hoisting, formatting). A flat per-node template engine produces ugly or wrong output; the
> format inevitably becomes a small **DSL** with conditionals, context queries, and sub-templates.
> Consequence for sequencing (§7): **you cannot design this DSL well before hand-writing at least two
> backends natively and seeing what decisions they actually make.** The DSL is *extracted* from working
> native backends, not guessed up front. The full-power-local tier (§3) covers what the DSL still can't.

### Build-dependency declaration
A backend or capability plugin declares the **target build-system dependencies** its output requires, and
the core threads them into the generated project:
- **C#:** SDK (`Microsoft.NET.Sdk`, `…Sdk.Web`), MSBuild properties (`<UseWindowsForms>`), `PackageReference`s.
- **TS:** npm `dependencies`/`devDependencies`, `tsconfig` lib entries (`"dom"`).
- **Python:** `pip`/`pyproject` requirements.

So a "C# desktop / WinForms" plugin says: *"bind WinForms; emit against it; the emitted project needs
`<UseWindowsForms>true</UseWindowsForms>` + these PackageReferences."* The core owns generating a buildable
project; the plugin owns declaring what that project must reference.

## 5. The faithfulness boundary (the honest cost)

The PRD's faithful-by-default (§3.C) and determinism (§3.D) promises apply to **core translation + the
portable std + the bundled C#/TS backend specs**. Beyond that:

- **Plugin output is the plugin author's contract**, not a core guarantee. A WinForms binding and some
  Qt-for-TS analogue target different environments and usually have *no* cross-target equivalent.
- Cross-target **conformance testing** (PRD §5) meaningfully applies to **portable code + capabilities with
  `actual`s on every tested target** — not to target-exclusive bindings, and not across two different
  downloaded backends unless they're declared equivalent.
- This is intended: parity lives in the portable core; the platform lives in plugins.

## 6. Workspace config, download & the trust model

`pg` reads a workspace config (`pgconfig.json`) from the cwd. It declares the **target
environments** (desktop/web/mobile/…) and the **plugins + versions** in use. From it the CLI resolves and
**downloads** the named plugins into a **shared cache**, then emits. **Availability** is resolved from the
config: a plugin symbol is usable only when its plugin is installed **and** the active target matches
**and** the active environment is among the plugin's declared environments — otherwise a clear compile
error (the §3.B "refuse out loud" rule, extended to platform APIs). Portable code (importing no plugin)
emits to every configured target; a module importing a plugin API is constrained to that plugin's
targets/environments, and the compiler can state "this module can only target {cs/desktop}".

**Trust model — what "download only data" buys, and what it doesn't:**
- ✅ **Eliminates compile-time arbitrary code execution** — the toolchain never runs downloaded code. This
  is the main win and the reason for declarative-only downloads.
- ⚠️ **Emitted code still runs later.** A malicious *declarative* plugin can still emit hostile target code
  (a binding that emits `File.Delete(…)` / exfiltration). Plugin output needs the same trust as any
  dependency — data-only shrinks the attack surface, it does not erase it.
- ⚠️ **"It's JSON" is not automatically safe.** Real safeguards: integrity verification (checksum/signature
  **pinned in a lockfile**), strict version pinning, zip-slip-/path-traversal-safe extraction into the
  shared cache, bounded/validated parsing (no billion-laughs/zip-bomb), and no executable bits.

Illustrative only (shape TBD; the `.json` extension keeps editors' schema/validation happy):
```jsonc
// pgconfig.json — workspace config
{
  "workspace": "fruitcake",
  "targets": {
    "cs": { "backend": "@polyglot/csharp@1.4.0", "environments": ["desktop"] },
    "py": { "backend": "@polyglot/python@0.3.1",  "environments": ["cli"] }
  },
  "plugins": {
    "@polyglot/winforms": "1.0.2",   // cs/desktop binding + build deps
    "@polyglot/web-dom":  "1.1.0"    // ts/web binding
  }
}
// "@polyglot/csharp" is bundled but still pinned; "@polyglot/python" is a DOWNLOADED
// declarative backend that adds Python with no core change. Resolved + verified
// against pgconfig.lock.json.
```

## 7. Sequencing — design for it now, build it incrementally

- **P2 (MVP) → P5:** a walking-skeleton slice end-to-end first (P2), then widen front-end (P3), semantics+IR
  (P4), and both backends (P5) — **C# and TS backends hand-written as native code in the core**. No DSL, no
  plugin loading, no platform APIs yet. (You must see two real, complete backends before designing the DSL.)
  **At P5, introduce the backend-interface seam** — a `Backend` abstraction selected via a registry instead
  of an `if/else` on the target. Backends stay compiled-in, but this is the interface the P9 declarative
  plugin API grows from; until P5 the two free emit functions are deliberately the simplest thing.
- **P7:** the portable std + the three §2 mechanisms (binding/replacement/capability) proven as first-party
  code against the two native backends.
- **P8:** dogfood (FruitCake), still on native backends.
- **P9 — Declarative backend engine + DSL:** *extract* the declarative backend format from the two native
  backends; re-express C# and TS as declarative specs; build the emit engine. Each spec also declares its
  **§3.E capability set** (both C#/TS = full §3.A). *Gate:* C#/TS emitted via declarative specs match the
  native backends' golden output byte-for-byte.
- **P10 — Plugin distribution + ecosystem:** `pgconfig.json` + download/cache/verify/version/lockfile;
  availability resolution by target+environment; **§3.E feature-capability gating** (usable §3.A surface =
  intersection of configured backends' capability sets; out-of-intersection use refused at compile time,
  distinct from a §3.B global refusal); build-dependency threading; the local full-power tier; and
  the proof — a **downloaded declarative Python backend** emits Python, and a downloaded binding plugin
  threads its PackageReferences. *Gate:* adding Python + a WinForms binding requires **no core change**,
  only config + downloads; off-target/-environment use **and** use of a feature outside the target
  intersection are each rejected with a clear, distinct diagnostic.

## 8. Open decisions

1. ~~Plugin mechanism in a C++ core~~ — **resolved:** declarative data interpreted by the core for the
   *downloaded* tier (no host runtime needed). The remaining sub-question is how *local full-power* plugins
   attach to a C++ core (native module vs. local scripting hook) — deferrable (§3).
2. **How expressive must the declarative backend DSL be?** The §4 challenge. Answered empirically by P9's
   extraction from two native backends; the local full-power tier covers the residue.
3. **Is the portable std core or the first official (bundled) plugin?** This note assumes a thin std *in the
   core*; could instead be a bundled `@polyglot/std`.
4. ~~Trust/security~~ — **modeled in §6** (declarative-only downloads + integrity/pinning/safe-extraction +
   the honest output-trust caveat). A registry + signing infrastructure is a P10+ detail.
5. **Config & lockfile shape** — `pgconfig.json` + `pgconfig.lock.json` (filenames fixed; internal schema
   + registry/version resolution still TBD). Defer to P10.

---

*This note refines PRD §4.4 (std library & platform-API strategy) and reinforces §4.3 (C++ host — a
self-contained native CLI means **plugin *users* need no SDK/runtime**, which only works because downloaded
plugins are data, not code). The §3 support/refuse contract and the faithfulness/determinism clauses
(§3.C/§3.D) are unchanged — plugins extend reach, they do not relax the core's promises about the code it
itself translates.*
