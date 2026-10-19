# Differential conformance tests

The crown-jewel test (PRD §5): a Polyglot program is emitted to **every** target, each is run, and their
stdout is asserted **identical** to the C# oracle (and to an optional committed `<name>.expected` golden).

## Layout
- `programs/*.pg` (and `programs/<dir>/entry.pg` for multi-module programs) — the runnable corpus.
- `run-conformance.ps1` — the merged, parallel runner: per program, one config-sourced multi-target
  `polyglot build`, one `csc /shared` oracle compile (`scripts/lib/OracleCompile.ps1`), then every runtime.
  `run-diff.ps1` / `run-python.ps1` / `run-php.ps1` are thin wrappers kept for muscle memory.
- `culture/PgCultureHook.cs` — the P40 culture leg's .NET startup hook (see below).
- `culture/fast-programs.txt` — the programs the `-Tier fast` synthetic-culture smoke runs.

## Gates inside the runner
- **Culture leg (P40 A3).** Each compiled C# oracle is re-run under `-Cultures tr-TR,nl-BE,sv-SE,fa-IR`
  (default) with `DOTNET_STARTUP_HOOKS` + `PG_CULTURE` set on that invocation only; its stdout must equal the
  invariant run. The hook asserts the culture took effect (fa-IR / sv-SE `NegativeSign != "-"`, tr-TR
  `"I".ToLower() == "ı"`, nl-BE decimal `,`) and exits 97 otherwise, so a host without ICU fails loudly.
  `pg-hostile` is a synthetic culture (an en-US clone with sign U+2212, decimal `,`, group `.`).
  `-Cultures none` turns the leg off.
- **PHP hostile precision.** PHP is re-run under `-d precision=17 -d serialize_precision=17` and must still match.
- **Grep gates.** No emitted file contains `__polyglot_unlowered_expr__`; no emitted `.cs` assigns
  `CurrentCulture`.
- Pinned PHP refusers, the anti-silent-drop check, per-program timeouts — see the script header.

## Run
Build the solution first (see `CLAUDE.md`), then:
```
pwsh tests/conformance/run-conformance.ps1                       # all four targets + the four cultures
pwsh tests/conformance/run-conformance.ps1 -Targets csharp -Cultures pg-hostile -Only numbers_text,parse
```
Requires `dotnet` (with ICU for the culture leg), and `node` / `python` / `php` for those targets.
Exit code 0 = all programs agree.
