# Coverage attribution — one `.pg` number from every target's test suite

One `.pg` source compiles to several targets, and **each target's suite covers a different part of it**,
because each has a different caller. A `.pg` line is covered when *any* target reached it. Measured in a
real consumer: `.pg` coverage went **93.95% → 98.12%** once the C# and TypeScript halves were overlaid.

Two steps, and the second is only needed for some targets:

1. Build with **`--origin-info`** (or pgconfig `"originInfo": true`) so the emitted code carries a link
   back to the `.pg`.
2. Run **`polyglot coverage remap`** over the report your test runner produced.

Then upload the `.pg`-keyed reports as you would any others. Merging them is the coverage service's job,
not Polyglot's — Polyglot's job ends at *"emit one `.pg`-keyed report per target."*

## What each target needs

| target | sink | needs `polyglot coverage`? |
|---|---|---|
| **C#** | `#line` directives, consumed by Roslyn → PDB → coverlet | **No remap.** The report already names `.pg` files. Run it anyway to validate and to complete the file set (below). |
| **TypeScript** | a v3 `.map` sidecar | yes |
| **Python** | a v3 `.map` sidecar | yes |
| **PHP** | a v3 `.map` sidecar | yes |

```
polyglot coverage remap <report> --target <name> --out <path>
                        [--format <fmt>] [--out-format <fmt>] [--root <dir>]
                        [--generated-dir <dir>] [--branch-arms]
```

Formats, read **and** written: `cobertura`, `lcov`, `istanbul`, `clover`. `--format` is sniffed by
default; `--out-format` defaults to the input format, so the tool hands your pipeline back what it already
consumes rather than becoming a format converter it was never asked to be.

## A worked recipe

```yaml
# C# — coverlet already reports on the .pg once PolyglotOriginInfo is on.
- run: dotnet test --collect:"XPlat Code Coverage" --results-directory coverage
- run: polyglot coverage remap coverage/**/coverage.cobertura.xml --target csharp
         --generated-dir obj --root . --out coverage-pg/csharp.xml

# TypeScript — vitest emits lcov; project it onto the .pg.
- run: npm run test:coverage
- run: polyglot coverage remap coverage/lcov.info --target typescript
         --generated-dir src --root . --out coverage-pg/typescript.info
```

`--generated-dir` is searched **recursively**, so pgconfig `include` rules may route one closure's output
across several trees without the tool needing to be told where each file went.

## Three rules worth knowing, because they decide the number

**The denominator is the mapped set, not the file.** A `.pg` line that maps to no generated line —
`class` heads, blank lines, declarations that carry no code — is **absent** from the report rather than
counted against you. One real module is 516 mappable lines of 831 physical, so a tool measuring against
raw line count would report a permanent, meaningless shortfall.

**A `.pg` file no test touched is reported at zero, not dropped.** If a module's twin never appears in the
coverage report, it still appears in the output with every mapped line at zero. Dropping it would make the
percentage read *higher* precisely when a module is least tested. The file set comes from the origin data
— the sidecars, or the `#line` directives — never from the report.

**Branch data is emitted count-only.** A line's branches are reported as a `(covered, total)` pair rather
than as individually identified arms, so only cobertura, clover and JaCoCo can carry it; lcov and istanbul
output is line-only.

That last one is deliberate and worth the paragraph. A coverage service merges arm *identities* across
reports, which is correct only when the identities are comparable — and across targets they are not: C#'s
come from coverlet over IL, TypeScript's from istanbul over JavaScript, for the same `.pg` line. When both
suites cover the **same** arm (the common case) merging the identities reports 2/2 where the truth is 1/2,
and is *correct* when they cover different arms — so the error is data-dependent and invisible. A count
under-credits instead, which is the honest direction.

`--branch-arms` opts back into arm-identified output. It is safe **only** for a single-target consumer,
where no cross-instrumenter merge can happen.

## Troubleshooting

**"no '*.map' sidecars under …"** — the build under test didn't run with `--origin-info`.

**"no file in … matched the origin data"** — on a `directive` target this almost always means
`--origin-info` was off for the build the tests ran against. Its only other symptom is a mysteriously
*lower* percentage, which is why the tool refuses instead of emitting an empty report.

**A path that doesn't resolve on the coverage service.** Emit repo-relative paths with `--root` pointing
at the repo. `scripts/verify-coverage-paths.ps1` checks a report against `git ls-files` using the same
rule the ingest applies: both suffix directions, case-insensitive, and **exactly one** candidate — an
ambiguous basename is stored unmatched and silently excluded from every number.

## C#: zero tooling, with three caveats

The `#line` mechanism is genuinely free — Roslyn writes the `.pg` into the PDB, and by the time coverlet
instruments IL there is no `.cs` coordinate left. Setup is one MSBuild property. But:

1. **Exclusion globs invert.** `ExcludeByFile=**/obj/**` matches the **PDB-recorded** path, so it stops
   matching generated code the moment that code claims a `.pg` origin. Zero *tooling*, but not zero
   *config*.
2. **A silent assembly drop.** coverlet checks each PDB document exists on disk; the default
   (`MissingAll`) saves you only because the `obj/…/*.cs` documents survive alongside the `.pg` ones. Set
   `ExcludeAssembliesWithoutSources=MissingAny`, or build on one machine and test on another, and the
   whole assembly vanishes from coverage without an error.
3. **Deterministic builds rewrite the path.** `DeterministicSourcePaths` + PathMap rewrite the `.pg` to
   `/_/…` exactly as they do the `.cs`. Keep `UseSourceLink=false` and `DeterministicReport=false` (both
   already default false).

One more, harmless but confusing: coverlet emits one `<class>` per type **and per closed generic
instantiation**, all sharing the `.pg` filename, so its own roll-up `line-rate` double-counts a shared
line. Every merging consumer (ReportGenerator, coverage.mintplayer.com) groups by filename and counts each
line once, so the numbers you actually read are right; only the rate attribute inside the raw file is off.
