# Differential conformance tests

The crown-jewel test (PRD §5): a Polyglot program is emitted to **both** targets, both are run, and their
stdout is asserted **identical**. This is what turns "keep two ports in sync" into a CI gate; it starts
tiny at P2 and grows to the full surface (P5) and the FruitCake physics (P8).

## Layout
- `programs/*.pg` — runnable Polyglot programs (the MVP subset). Keep output **integer/string only** for
  now: `bool` (`True` vs `true`) and `float` formatting diverge between `Console.WriteLine` and
  `console.log` — that faithfulness work is P6.
- `run-diff.ps1` — for each program: `polyglot build` → compile+run the C# (`dotnet`) and run the TS
  (`node` type-stripping) → compare stdout.

## Run
Build the solution first (see `CLAUDE.md`), then:
```
pwsh tests/conformance/run-diff.ps1
```
Requires `dotnet` and `node` on PATH (Node 18.6+/22+ for `.ts` type-stripping; verified on Node 24).
Exit code 0 = all programs agree across targets.
