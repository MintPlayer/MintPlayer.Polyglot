# Design note — Plugins, targets & environments

> **Status:** Design note v0.1 · 2026-06-28 · captures a decision taken during P1 review; refines PRD §4.4.
> Not a milestone deliverable — the plugin system proper lands post-P7 (see §7 below). This note exists so
> every milestone before it is built *toward* this architecture instead of away from it.
>
> **Working interpretation (correct any paragraph):** this note writes up the architecture agreed in the
> P1 discussion — a pure translator core with *all* target/environment/SDK knowledge pushed into full-power,
> installable plugins (backends included). Where it states something as decided that you meant differently,
> it's a doc edit, not a rebuild.

---

## 1. The premise: the core is a pure translator

The core compiler knows exactly two things and nothing else:

1. **The Polyglot language** — the §3.A surface (operators, classes/records, generics, pattern matching,
   iterators, exceptions, …), lexed/parsed/type-checked and lowered to the one typed IR.
2. **A minimal portable std** — the handful of types guaranteed identical everywhere (collections, string,
   math, iterators), written in `.pg` and compiled like any other source.

It has **no knowledge of any target language's platform, SDK, or environment APIs** — not `window`, not
`System.Windows.Forms`, not `fs`, not the DOM. It does not even hardcode *how* to emit C# or TypeScript
beyond the language-level lowering. Everything beyond the pure language + portable std is a **plugin**.

This is the scope line, sharpened: the transpilers that died drowned in *platform surface* baked into the
compiler. Here the core's platform surface is **zero by construction** — it cannot drown in what it does
not contain. The unbounded part lives in opt-in, external plugins.

## 2. Three mechanisms a plugin provides

| Mechanism | What it does | Analogue |
|---|---|---|
| **Binding** | Declares the *shape* of an existing target API and emits calls to it (`window`, `WinForms`, `fs.readFile`). Target-scoped. | TS `.d.ts`, Haxe extern |
| **Replacement** | Maps a *portable* API to **different** idiomatic code per target (`List.map` → `.Select(…)` / `.map(…)`). | Fable Replacements |
| **Capability** (`expect`/`actual`) | Portable code names an abstract need (`nowMillis()`); each target's plugin supplies the concrete `actual`. | Kotlin Multiplatform |

A plugin exposes these through a **public API** that `.pg` code imports and calls like any other module.

## 3. Plugins are full-power and declare build dependencies

Plugins may run arbitrary logic to produce their emission (decision: *full power from the start* — maximum
flexibility; the faithfulness boundary in §5 is what keeps this honest). Crucially, a plugin also declares
the **target build-system dependencies** its output requires, and the core threads them into the generated
project:

- **C# target:** the SDK to use (`Microsoft.NET.Sdk`, `Microsoft.NET.Sdk.Web`, …) and `PackageReference`s
  (e.g. `System.Windows.Forms`, a NuGet package + version).
- **TypeScript target:** npm `dependencies`/`devDependencies`, `tsconfig` lib entries (`"dom"`).
- **(Future) Python target:** `pip`/`pyproject` requirements.

So a "C# desktop" plugin says, in effect: *"I bind WinForms; emit against it; and the project I emit into
needs `<UseWindowsForms>true</UseWindowsForms>` + these PackageReferences."* The core owns generating a
buildable project; the plugin owns declaring what that project must reference.

## 4. The backends are themselves plugins

A target language (C#, TypeScript, later Python) is the **transpiler plugin** for that target — it owns the
IR→source pretty-printer and the base build-project scaffold. Environment/SDK capability plugins (DOM,
WinForms, Node `fs`) layer on top of a target plugin and add bindings + dependencies.

> **Implementation reality (open decision, §7):** "backend = plugin" is the *architecture*, not necessarily
> the *initial implementation*. Through P4/P5 the C# and TS backends are expected to be **first-party,
> compiled into the core**, but written **behind the same backend/plugin interface** third-party plugins
> will use — so the seam exists from the start without paying for dynamic loading before there are two
> backends to generalize from. How full-power *third-party* plugins load into a C++ core (native ABI vs. an
> embedded scripting / IR-rewrite engine) ties into the PRD §4.3 implementation-language fork and is
> deliberately left open here.

## 5. The faithfulness boundary (the honest cost)

The PRD's faithful-by-default promise (§3.C) and determinism honesty (§3.D) apply to **core translation +
the portable std**. A full-power plugin can emit whatever it likes, so:

- **Plugin output is the plugin author's contract**, not a core guarantee. The core cannot promise that a
  binding to `WinForms` and one to some Qt-for-TS analogue "behave identically" — they target different
  environments and frequently *have no cross-target equivalent at all*.
- Cross-target **conformance testing** (the crown jewel, §5 of the PRD) therefore meaningfully applies to
  **portable code + capabilities with actuals on every tested target** — not to target-exclusive bindings.
- This is *fine and intended*: most platform code is target-exclusive by nature. The portable core is where
  parity lives; plugins are where the platform lives.

## 6. Workspace config: targets, environments, plugins

A workspace declares what it targets and what it pulls in. The compiler resolves **availability** from this:
a plugin symbol is usable only when its plugin is installed **and** the active target matches **and** the
active environment is among the plugin's declared environments — otherwise a clear compile error. Portable
code (importing no plugin) compiles to every configured target; a module importing a plugin API is
constrained to that plugin's targets/environments, and the compiler can state "this module can only target
{cs/desktop}".

Illustrative only (format TBD):

```toml
# polyglot.toml — workspace config
[workspace]
name = "fruitcake"

[targets.cs]
plugin       = "@polyglot/csharp"     # the C# transpiler plugin
environments = ["desktop"]

[targets.ts]
plugin       = "@polyglot/typescript"
environments = ["web"]

[plugins]
"@polyglot/web-dom"  = "^1.0"         # window/document/history — ts, web
"@polyglot/winforms" = "^1.0"         # System.Windows.Forms     — cs, desktop
```

```toml
# plugin manifest (inside @polyglot/winforms)
[plugin]
name         = "@polyglot/winforms"
target       = "cs"
environments = ["desktop"]

[plugin.build.cs]                      # threaded into the emitted .csproj
sdk               = "Microsoft.NET.Sdk"
properties        = { UseWindowsForms = "true" }
packageReferences = ["System.Windows.Forms@8.0.0"]
```

## 7. Sequencing — design for it now, build it incrementally

This architecture is foundational but must **not** be built up front (that would block the incremental
P2→P5 path on a speculative plugin host). Recommended ordering:

- **P2–P5:** C# and TS backends as first-party, compiled-in components, written behind a backend interface
  that is *designed to become* the plugin API. No dynamic plugin loading yet. No platform APIs yet.
- **P7 (reframed):** the portable std + the **`expect`/`actual` capability mechanism** + the first real
  **binding/replacement** path — i.e. prove the three §2 mechanisms work *as first-party code* against the
  two backends.
- **New milestone (≈P9, post-dogfood):** the plugin system proper — external plugin loading, the workspace
  config, availability resolution by target+environment, build-dependency threading, and the full-power
  plugin mechanism decision (§4 / PRD §4.3). Generalized from two working backends, not guessed.

## 8. Open decisions (to resolve before the plugin milestone)

1. **Plugin mechanism in a C++ core** — native ABI, embedded scripting, or a declarative IR-rewrite DSL?
   (Interacts with PRD §4.3.)
2. **Is the portable std core or the first official plugin?** This note assumes a thin std *in the core*;
   could instead be `@polyglot/std`.
3. **Trust/security of full-power plugins** — they run at compile time and emit code; a sandboxing/trust
   story is needed before any third-party registry.
4. **Config format & resolution** — `polyglot.toml` vs JSON; how versions/registries resolve (defer until
   the milestone).

---

*This note refines PRD §4.4 (std library & platform-API strategy). The §3 support/refuse contract and the
faithfulness/determinism clauses (§3.C/§3.D) are unchanged — plugins extend reach, they do not relax the
core's promises about the code it itself translates.*
