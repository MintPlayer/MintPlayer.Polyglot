# Design note — Plugins, targets & environments

> **Status:** Design note v0.2 · 2026-06-28 · refines PRD §4.4 and §4.3. Captures decisions taken during
> P1 review. Not a milestone deliverable — the plugin system proper lands at P9/P10 (§7). This note exists
> so every milestone before it is built *toward* this architecture instead of away from it.
>
> **v0.2 change:** plugins are now **declarative data interpreted by the core** (not loadable executable
> code), downloaded + versioned via a workspace config; **target backends are themselves declarative
> plugins**; and there are **two plugin tiers** (declarative-downloaded vs. full-power-local). This
> supersedes v0.1's "full power from the start."
>
> **v0.3 change (2026-07-01):** distribution is resolved — **no per-plugin executables**; a plugin is a
> declarative artifact fetched from a feed by name+version, and **`polyglot install`** is the single trusted
> writer of a **global per-user registry** (§6.1). This settles the "distribute an .exe per plugin that
> self-registers" idea against the declarative-only stance.
>
> **v0.4 change (2026-07-01):** **backends become pure-JSON data plugins too** (§6.2) — a 4-agent investigation
> established the emission Hooks flatten to a bounded, RCE-safe JSON DSL (§4.10 / `backend-spec.md` §6), so a
> language is an installable data plugin, not compiled-in C++. Reframes P9's "irreducible 30%" as ≈85% data /
> ≈95%+ with a few fixed Core primitives / <5% target-limits-the-gate-refuses. Slice plan: PLAN §P18.

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

> **As built (P30, 2026-07-16 — issue #30):** the download half of this promise is now real.
> `polyglot build`/`check`/watch/LSP auto-resolve every `pgconfig.json` dependency inside the exe —
> `file:` in place → in-box when the CLI's lockstep version satisfies the range → the verified
> versioned cache when `pgconfig.lock.json` pins → the npm registry HTTP API (abbreviated packument,
> `maxSatisfying`, SRI verify, in-exe gunzip+untar, `validateBackend`) — no npm process, no system
> tar, no lifecycle scripts. `environments` gating (below) remains future work. Design details that
> changed in the building are annotated in §6.1/§6.3; full record: `docs/prd/issue-30-plugin-autodownload/`.

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

The full resolved schema is **§6.3**; the shape in brief (the `.json` extension keeps editors' schema/validation happy):
```jsonc
// pgconfig.json — workspace config (see §6.3 for the full field spec)
{
  "root": ".",
  "dependencies": {                       // ALL plugin packages this codebase needs (npm name → version)
    "@polyglot/csharp":   "1.4.0",        //   a LANGUAGE plugin (a target you compile TO)
    "@polyglot/python":   "0.3.1",        //   a downloaded language plugin — adds Python, no core change
    "@polyglot/winforms": "1.0.2",        //   a LIBRARY/binding plugin (code you import) — cs/desktop + build deps
    "std.io": "*", "std.math": "*"        //   std library plugins
  },
  "targets": ["csharp", "python"],        // which language(s) to emit (must be language deps); overridable by --target
  "lib": ["io", "math"],                  // ambient (no-import) LIBRARY modules — a SUBSET of dependencies
  "environments": { "csharp": ["desktop"], "python": ["cli"] }
}
// Integrity per dependency is pinned in pgconfig.lock.json. "@polyglot/csharp" is
// bundled in-box but still listed + pinned (uniform resolution). §6.3 has the details.
```

### 6.1 Distribution — `polyglot install` + a global registry (resolved 2026-07-01)

A plugin is a **declarative artifact** — a bundle of `.json`/`.pg` (backend spec, bindings, type maps,
capability set, build-dependency declarations) — published to a **feed** and fetched by name+version. There
is **no per-plugin executable.** (The "ship an .exe per plugin that writes a shared registry when run" idea
was considered and rejected: it reintroduces fetch-and-run — the precise attack surface §6 refuses — and
letting every plugin scribble the shared registry invites concurrent-write/corruption/trust problems.)
Instead:

- **`polyglot install <plugin>[@version]` is the single trusted writer.** It resolves the plugin from the
  feed, verifies integrity, extracts it zip-slip-safe into the **shared cache**, and records it there.
  Core stays IO-free; the CLI/LSP layer reads it, exactly like `pgconfig.json`.
  > **Superseded at P30:** the separate global `registry.json` index was **not built** — the versioned
  > cache directory (`<cache>/<npm-name>/<version>/{polyglot-plugin.json, meta.json}`, where `meta.json`
  > carries resolved URL + SRI integrity + a manifest hash re-verified on every load) *is* the machine-wide
  > installed set, and the per-project lockfile carries the pins; a second index would be a second source
  > of truth with no consumer. Cache location as implemented: `%LOCALAPPDATA%\polyglot\plugins` (Windows) /
  > `$XDG_DATA_HOME` or `~/.local/share/polyglot/plugins` (Linux) / `~/Library/Application Support/polyglot/
  > plugins` (macOS), overridable wholesale via `POLYGLOT_CACHE`. And since build-time resolution now
  > downloads by itself, `install` is an optional cache pre-warmer / lock writer (`--update` re-resolves
  > past the lock), no longer a required manual step.
- **Two-level resolution.** The versioned cache is the machine-wide *installed* set; the per-project
  `pgconfig.json` pins *which* plugins + versions a workspace actually uses; `pgconfig.lock.json` pins
  integrity per §6.
- **Feed — leading candidate is an existing package registry (npm).** Reusing npmjs.com buys versioning,
  immutable integrity hashes, and a global CDN for free; a plugin is just a package whose payload is the
  declarative bundle. **Consumed as data only** — Polyglot fetches+extracts the tarball via the registry
  HTTP API and **never runs npm lifecycle scripts** (running them would reintroduce fetch-and-run). A generic
  URL / file-hosting feed (a bundle + a hash pinned in the lockfile) is the fallback and keeps Polyglot
  **feed-agnostic**: a registry entry records a *source + integrity*, npm being one source kind. An
  own-registry + signing service is a later detail (open #4).
- **Editor tie-in.** The LSP reads the same global registry, so the `polyglot/targets` list (PLAN §P17
  deferred / §P10) enumerates installed backends and the P17 "Show Generated Output" preview picks up a
  plugin's target with **no client change**.

### 6.2 The language-plugin package (backends as pure JSON — 2026-07-01; PRD §4.10 / PLAN §P18)

A **backend** plugin is an npm package whose payload is data only (no `.js`/`bin`/lifecycle scripts are ever
consulted — see the trust model above). It contains a `polyglot-plugin.json` manifest referencing:
- **`backend.spec`** — the JSON emission DSL for the target (§4.10 / `backend-spec.md` §6: the `Rule` table per
  IR node kind + precedence/type/literal/naming tables). This is what makes a backend *data*, not code.
- **`backend.capabilities`** — the §3.E `Feature` set the target supports (the Core intersects it across all
  configured targets; out-of-set use is refused naming the capability + target). Tri-state where it matters
  (`extensionMethods: native | free-function | false`).
- **`backend.{targetId, displayName, fileExtension, commentSyntax}`** — identity + editor metadata.
- **`externTypes`** — the `extern class` → target-type + construction templates (the `ir::ExternType` data).
- **`buildDeps`** — target build-system deps to thread into the emitted project (C# SDK/PackageReference, TS
  npm deps + tsconfig lib, Python requirements) — §4 "Build-dependency declaration".
- **`stdModules`** — any bound std `.pg` modules the plugin ships (a new target brings *its own* `actual`
  arms — this is how the ~90 hardcoded `actual(target)` arms in `compiler.cpp` stop being a per-target Core
  edit; PLAN §P18 slice 7).

```jsonc
// polyglot-plugin.json
{ "schema": 1, "id": "@polyglot/kotlin", "version": "0.1.0", "kind": "backend",
  "backend": { "targetId": "kotlin", "displayName": "Kotlin", "fileExtension": ".kt",
               "commentSyntax": { "line": "//", "block": ["/*","*/"] },
               "capabilities": { "extensionMethods": "native", "operatorOverloading": true, "async": true, … },
               "spec": "backend/spec.json", "externTypes": "types/externtypes.json" },
  "buildDeps": "builddeps.json",
  "stdModules": { "std.io": "std/std.io.pg", "std.math": "std/std.math.pg" } }
```
The registry entry (§6.1) for a backend carries `targets: [{id, displayName, fileExtension}]` + the capability
set + cache path + integrity hash — so `--target <name>` resolves name→registry→cache→spec bytes→`BackendHandle`
(Core interprets; the CLI does the IO), and the editor `polyglot/targets` (§6.1 tie-in) needs no extra source.
~~The built-in C#/TS/Python migrate to this exact shape as **in-box specs embedded in the binary**~~ —
**superseded 2026-07-02 (user decision, `json-plugins.md` scope note): the CLI embeds no target specs at
all.** The first-party three are **ordinary plugin packages** developed in this repo (`plugins/<target>/`)
and published to npm; every consumer resolves them through `pgconfig.json` `dependencies` (local `file:`
path → lockfile-pinned cache → registry) exactly like a third-party target. `kind: "binding"|"std"`
plugins omit the `backend` block and carry only `stdModules`/`externTypes`/`buildDeps` — already fully
expressible today (P7/P8/P10 mechanisms interpret exactly this data).

**§6.2 extended (2026-07-02):** the P19 investigation finalized this format in `json-plugins.md` §5 —
additions, not contradictions: `requiresCore` (semver of the interpreter contract, enforced at load), split
rule files (`backend/expr.json`/`stmt.json`/`decl.json` + `precedence.json`), **tri-state capabilities**
(`native|emulated|false` in `capabilities.json`), **std overlays** (`std/*.overlay.json` — member-keyed
`{module → member → template}` arms replacing whole bound `.pg` modules, so `ir::Bound`/`ExternType`'s fixed
cs/ts/py fields collapse to single templates selected at link time), a `preludes/` section for `require`d
target-code fragments, and the full load-time validation catalog. Read `json-plugins.md` first for anything
plugin-format related; this section stays as the distribution/registry context.

### 6.3 The `pgconfig.json` schema (resolved 2026-07-01)

The project manifest, parsed in the CLI/LSP layer (Core stays IO-free). It evolves from the minimal `{root, lib}`
shipped at P16 to the full form below at P10/P18 — additively, so today's files keep working.

- **`root`** *(string)* — module-resolution root (shipped, P16). Relative imports (`"./x"`) resolve from the file;
  bare logical imports (`"a.b"`) from `root`.
- **`dependencies`** *(map: npm-package-name → version-range)* — **every plugin package this codebase needs.** The
  umbrella dependency set; the lockfile pins each one's integrity. Two *kinds* of entry (distinguished by the
  installed plugin's `kind`, not by pgconfig syntax — see §6.2):
  - a **language plugin** — *what you compile TO* (`@polyglot/csharp`, `@polyglot/python`, `@polyglot/kotlin`…). A
    target choice.
  - a **library/binding plugin** — *what you `import` and call* (`std.io`, `@polyglot/winforms`…). Ordinary code deps.
  `polyglot install` (no arg, in a project) installs everything here not yet in the global registry, writing the
  lockfile. This is the field the user flagged as missing; it is the P18/P10 addition.
- **`targets`** *(list of language-plugin target ids)* — which language(s) to **emit**. Must each resolve to a
  *language* plugin in `dependencies`. A per-build `--target <name>` overrides/selects among them. (Target selection
  is distinct from dependency: you *depend on* a language plugin, and separately *choose* to emit to it — a codebase
  can depend on several and emit to any/all.)
- **`lib`** *(list of ambient module names)* — LIBRARY modules auto-imported into every file with no `import`
  statement (shipped, P16: `["io","math"]` → `std.io`/`std.math`). **A subset of `dependencies`**: every `lib` entry
  must resolve to an installed library dependency (or a built-in). `lib` never contains language plugins. Ambient +
  lowest-priority: a `lib` name loses silently to any user decl or explicit import of the same name.
- **`environments`** *(map: target id → environment list)* — desktop/web/mobile/cli availability gating (§6). A
  plugin symbol is usable only when its plugin is installed **and** the active target matches **and** the active
  environment is among the plugin's declared ones — else a clean §3.B-style refusal.
- **`forbiddenIdentifiers`** *(map: target id or `"*"` → identifier list; bare array = `"*"` sugar)* — added
  2026-07-02 (`json-plugins.md` §7): **per-project** identifier bans on top of what each language plugin already
  declares (its `identifiers` manifest block carries the target's keywords/escape strategy + reserved scaffolding
  names + runtime globals). A declared identifier matching an entry refuses with a targeted diagnostic via the
  per-target `checkReservedNames` pass. Applies to **identifiers only** — never to text inside string literals,
  interpolation chunks, comments, or `extern("…")` templates (the check runs over sema's symbol tables, not source
  text). Example: `{ "*": ["temp"], "csharp": ["Program"] }`.

**Two-level resolution (as built at P30):** the **versioned cache** (§6.1) = the machine-wide *installed* set;
**`pgconfig.json` `dependencies`** = which packages (+ version ranges: exact / `^` / `~` / `>=` / `latest` /
`file:`) *this* project uses; **`pgconfig.lock.json`** pins `version` + `resolved` URL + SRI `integrity` per
dependency (re-verified on load; lock-satisfied builds do **zero** network I/O). A dependency named but not
cached is **downloaded at build time** — the old "run `polyglot install <name>@<version>`" stop is gone; a
`targets` entry no dependency provides gets a diagnostic naming the `dependencies` fix.

**The load-bearing invariant (why this is RCE-safe):** a plugin — language or library — is **data the trusted Core
interprets**, never an executable the toolchain runs. The developer writes `.pg`; the CLI resolves the chosen
`targets` to their language plugins' JSON specs (§6.2) and the **Core's interpreter** (§4.10) emits the target
source by interpreting that data. The plugin is a passive spec, not a transpiler — that is precisely what keeps
`polyglot install` + transpile safe against fetch-and-run RCE.

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
5. ~~Config & lockfile shape~~ — **resolved (§6.3, 2026-07-01):** `pgconfig.json` = `{root, dependencies,
   targets, lib, environments}` + `pgconfig.lock.json` for integrity pinning; `dependencies` is the umbrella
   plugin set (language *and* library plugins), `targets` selects what to emit, `lib` ⊆ dependencies is the
   ambient library subset. **Distribution resolved (§6.1):** no plugin executables; `polyglot install` + a global
   per-user registry; feed candidate npm (consumed data-only), URL fallback, feed-agnostic. Remaining sub-question:
   the concrete feed choice + own-registry/signing (own-registry deferred, open #4).

---

*This note refines PRD §4.4 (std library & platform-API strategy) and reinforces §4.3 (C++ host — a
self-contained native CLI means **plugin *users* need no SDK/runtime**, which only works because downloaded
plugins are data, not code). The §3 support/refuse contract and the faithfulness/determinism clauses
(§3.C/§3.D) are unchanged — plugins extend reach, they do not relax the core's promises about the code it
itself translates.*
