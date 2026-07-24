# Authoring a target plugin

A Polyglot output language is **one JSON file** — `polyglot-plugin.json` — loaded and validated at
runtime. No compiler change, no C++, no build step: the CLI is a pure engine with zero compiled-in
backends, and even the first-party C#/TypeScript/Python targets are ordinary plugins in this repo's
`plugins/` directory. PHP was added exactly this way (`plugins/php/polyglot-plugin.json`), and it is
the canonical worked example this guide quotes from.

The emission DSL is deliberately **bounded and non-Turing-complete**: rules are data interpreted by
the engine, there is a fixed catalog of builtin functions, and helper-rule recursion is depth-capped.
A plugin cannot run code; it can only describe output shapes. Anything a target genuinely cannot
express is declared as an unsupported capability and refused at compile time with a diagnostic —
never silently miscompiled.

## 1. What a plugin is

The manifest's top-level keys (all of them):

```json
{
  "schema": 1,
  "name": "php",
  "capabilities": { "async": false, "...": "..." },
  "spec": { "...": "..." },
  "std": { "...": "..." },
  "rules": { "...": "..." },
  "fileExtension": ".php"
}
```

| Key | Required | What it is |
|---|---|---|
| `schema` | convention | Manifest schema version; first-party plugins declare `1`. |
| `name` | yes | The target name (`--target <name>`); must equal `spec.name`. |
| `capabilities` | as needed | Tri-state feature stances: `"native"` / `"emulated"` / `false` (§3 below). |
| `spec` | yes | The declarative per-target data tables (§3 below). |
| `std` | as needed | Per-target bodies for the std library skeletons (`print`, `List`, `Math`, …). |
| `rules` | yes | The emission rule tables; `Program` and `Type` are mandatory. |
| `fileExtension` | no | Output extension; defaults to `.<name>`. |
| `crossDirImports` | no | `true` when the target's emitted import specifiers may span directories (the compiler then hands the import rules a full relative specifier like `../shared/name` instead of a bare basename, and a pgconfig `include` layout may split an import closure across directories). Default `false`: all files of one closure must share an output directory — the CLI refuses a split. TS declares it; Python/PHP don't (dot-package / include semantics). |

Plugins register through `loadBackend()`, which strictly parses and validates the whole artifact
(§5) — a malformed plugin fails to load with a named error, never degrades output.

## 2. Quick start

1. Copy `plugins/php/` to a new folder, e.g. `my-plugin/`.
2. Edit `polyglot-plugin.json`: set the top-level `name` **and** `spec.name` to your target's name
   (they must match), set `fileExtension`, then start reshaping the rules.
3. Wire it into a project — either of:
   - a `pgconfig.json` dependency (resolved in place, ideal while iterating):
     ```json
     { "dependencies": { "mylang": "file:./my-plugin" }, "targets": ["csharp", "mylang"] }
     ```
   - or install it into the user cache: `polyglot install ./my-plugin`
     (validates first — an invalid plugin is never cached — then copies to
     `%LOCALAPPDATA%\polyglot\plugins\<name>\`).
4. Build: `polyglot build foo.pg --target mylang` — or a bare `polyglot build foo.pg`, which emits
   every target in pgconfig's `targets` list.

Resolution order for a target name: the `plugins/` directory next to the `polyglot` executable
(loaded at startup) → pgconfig `dependencies` `file:<dir>` entries (relative to the pgconfig.json's
directory, expecting `<dir>/polyglot-plugin.json`) → the user cache. An unresolved name gets an
error that names those channels.

## 3. The manifest, block by block

### 3.1 `spec` — the declarative data tables

Every key `loadBackendSpec` reads (all optional except `name`; defaults shown where they exist):

| Key | Shape | Meaning |
|---|---|---|
| `name` | string | Must equal the plugin `name`. |
| `scalarType` | map | `.pg` scalar → target spelling (C#: `"i32": "int"`, `"i64": "long"`). Missing = emit the `.pg` name. |
| `intSuffix` | map | Integer-literal type → suffix (C#: `{"i64": "L", "u64": "UL", "u32": "U"}`). |
| `binaryOp` | map | Operator → target spelling for divergent ones (TS: `{"==": "===", "!=": "!=="}`); missing = verbatim. |
| `wrapInt` | map | §3.C overflow faithfulness: width → wrap template, `$x` = the expression. PHP: `"u8": "(($x) & 0xff)"`. No row = unwrapped. |
| `preludes` | map | Key → text prepended once when required — by `{"fn":"require"}` in a rule, or automatically for key `asyncEntry` when the entry fn is async (Python's `_pg_idiv`, `asyncio.run`). |
| `delimited` | map | Node → `{"open","sep","close"}` bracketing (PHP: `"tuple": {"open":"[", "sep":", ", "close":"]"}`). |
| `blockStyle` | string | `"bracesAllman"` (C#) \| `"bracesKnR"` (default; TS, PHP) \| `"colonIndent"` (Python). |
| `stmtEnd` | string | Statement terminator; default `";"` (Python: `""`). |
| `throwKeyword` | string | Default `"throw"` (Python: `"raise"`). |
| `rethrow` | string | The value-less rethrow statement, terminator included; default `"throw;"` (PHP: `"throw $__e;"`). |
| `trueLit` / `falseLit` / `nullLit` | string | Literal spellings; defaults `true`/`false`/`null` (Python: `True`/`False`/`None`). |
| `escapes` | map of maps | Named escape maps for literal contexts (source sequence → replacement, longest match wins); applied via `{"fn":"escape"}`. |
| `tables` | map of maps | Named lookup tables for `{"fn":"table"}` / `{"fn":"subst"}`. Two are read by the shared statement walk: `localDecl` (`mutable`/`const` templates, `$x` = the name — PHP `"$$x"`, TS `let $x`/`const $x`) and `yield` (`value`/`empty` — PHP `"yield $x;"` / `"return;"`). TS also carries `opMethod` and `bigNarrow`. |
| `generics` | object | `{"style", "boundsIntro", "boundsSep", "erase"}`. `style`: `"whereClauses"` (C# `<T> where T : A`) \| `"inlineBounds"` (TS `<T extends A>`) \| `""` (none). `erase` lists compile-time-only bounds (`INumber`). |
| `wrapAtom` | object | `{"recv": [...], "unary": [...]}` — which child kinds get parenthesized as a member/call receiver or unary operand. Vocabulary: `"binary"`, `"unary"`, `"cast"`, `"cond"`, `"binaryScalar"`. |
| `identifiers` | object | See §3.4. |

### 3.2 `capabilities` — tri-state, and the coverage contract

Each entry is a feature name mapped to `"native"`, `"emulated"`, or `false` (JSON `true`/`false`
normalize to `"native"`/`"false"`). A feature **absent** from the map is supported. Only `"false"`
gates: a program using that feature is **refused at compile time** with a §3.E diagnostic — the user
gets an error naming the feature, never broken output.

The compile-gate feature names: `extensionMethods`, `operatorOverloading`, `properties`,
`iterators`, `patternMatching`, `closures`, `exceptions`, `disposal`, `inheritance`, `async`,
`blockLambdas`, `withExpressions`, `mutableRefClasses`, `fixedWidthIntegers`, `utf16Strings`,
`propertySetters`.

**Keyed refinements (P37 slice 0).** A capability may also be a `parent:child` key from the closed,
load-validated vocabulary — an unknown key is a **load error**, never silently ignored. A sub-key
with no explicit entry inherits the bare parent's stance (the umbrella rule); an absent parent is
supported. Current keyed entries:

- `interfaces:runtimeIdentity` — interfaces exist as testable runtime types (`is`/`as`/typed match).
  TS declares `false` (interfaces erase); Python overrides its `"interfaces": "emulated"` umbrella
  with an explicit `"native"`.
- `operatorOverloading:{arithmetic,comparison,eq,indexers,conversion}` — graded operator support.
  PHP declares the umbrella `false` plus `:eq` and `:indexers` `"native"` (`->eq(...)`,
  `->get`/`->set`).
- `attributes:target.{type,method,function,field,param}` — where a Tier 1 pass-through attribute may
  attach on this target. TS declares `function` and `param` false (TC39 has neither); Python declares
  `field` and `param` false. Tier 2 portable metadata needs **no capability at all** — the compiler
  both writes and reads its data, so a plugin never sees it.

**Constant spellings + trait flags (P37).** The Core is a pure engine: it never compares target names
(it cannot know what languages exist). Anything lowering must bake into pre-rendered text or gate a
pass on comes from `spec` properties instead:

| Property | Default | Meaning |
|---|---|---|
| `enumMemberOp` | `"."` | how a constant enum member spells (`Color.Red` vs PHP's `"::"`) |
| `constArrayOpen` / `constArrayClose` | `"["` / `"]"` | constant array delimiters (C#: `"new[] {"` / `"}"`) |
| `linksWithoutImports` | `false` | modules link by compilation into one unit — no import statements (C#) |
| `forbidsShadowedLocals` | `false` | a nested-scope local may not reuse an outer local's name → the scope-legalization pass runs (C#, CS0136) |
| `expressionOnlyLambdas` | `false` | lambdas are expression-only → block lambdas hoist to local functions (Python) |

Capabilities also serve the **anti-silent-drop coverage contract** enforced at load time: every IR
construct the compiler can produce must have a rule, *or* the plugin must declare its stance via the
paired capability. Core constructs (no capability can excuse them — a rule is mandatory):

> `Int`, `Float`, `Bool`, `Null`, `Str`, `Char`, `Var`, `This`, `Extern`, `Call`, `Member`,
> `MethodCall`, `Index`, `Cond`, `ListLit`, `Tuple`, `New`, `Unary`, `Cast`, `Interp`, `Binary`,
> `ForStmt`, `Program`, `Type`, `EnumDecl`, `RecordDecl`, `ClassDecl`, `MethodDecl`, `FunctionDecl`

Capability-paired constructs (rule **or** a declared `"false"`/`"emulated"` stance):

| Rule | Capability |
|---|---|
| `MakeCase`, `Match`, `UnionDecl` | `patternMatching` |
| `With` | `withExpressions` |
| `Await` | `async` |
| `Lambda` | `closures` |
| `TryStmt` | `exceptions` |
| `InterfaceDecl` | `interfaces` |

So PHP, which ships no `Match` rule, declares `"patternMatching": false` — and a `.pg` `match` under
`--target php` is a clean refusal. Python declares `"interfaces": "emulated"` (duck typing: no
`InterfaceDecl` rule needed, but the stance is explicit) and `"blockLambdas": false`. Claiming
`"native"` while shipping no rule is itself a load error.

### 3.3 `std` — per-target bodies for the standard library

Core ships only std *skeletons* (`print`, `List`, `Math`, `Error`, `string`, `i32.parse`, …) with
zero target arms. Your plugin supplies them, organized by module for readability; member keys are
unique across the std surface and get flattened at load:

```json
"std": {
  "std.io":   { "print": "echo is_bool($x) ? ($x ? \"true\" : \"false\") : $x, \"\\n\"" },
  "std.core": { "i32.parse": "(intval($0) & 0xffffffff)", "f64.parse": "floatval($0)" }
}
```

Three template flavors:

- **Bound members** (`List.add`, `Math.min`, `Error.message`, `i32.parse`): call-site templates.
  `$this` = the rendered receiver, `$0`,`$1`,… = the rendered arguments. A template of the form
  `$this = …` emits a **receiver assignment** instead of an expression — Python's
  `"List.clear": "$this = []"`.
- **`<Type>.type`** maps an extern class's type spelling (`$0`,`$1` = rendered type args — C#:
  `"List.type": "global::System.Collections.Generic.List<$0>"`); **`<Type>.constructor`** maps its
  construction (`$T` = the mapped type, `$0`,… = ctor args — Python: `"List.constructor": "[]"`).
- **`expect` functions** (`print`): the template is the target-language *body* of the synthesized
  `actual`, referencing the fn's parameters by name (PHP's `print(x)` body reads `$x` because PHP
  spells the variable that way; Python's reads bare `x`).

A std member you don't overlay is refused at its use site for your target — un-overlaid calls never
silently emit garbage.

### 3.4 `identifiers` (inside `spec`) — keywords, reserved names, globals

```json
"identifiers": {
  "keywords": ["class", "function", "yield", "..."],
  "escape":   { "strategy": "suffix", "with": "_" },
  "mangle":   { "replace": "$", "with": "_" },
  "reserved": ["__e", "__opt*", "__w*"],
  "globals":  ["str"]
}
```

- `keywords` + `escape`: a `.pg` identifier colliding with a target keyword is respelled per the
  strategy — `"prefix"` (C#: `@switch`), `"suffix"` (Python/PHP: `global_`), or `""`/absent = no
  escape mechanism, in which case list those words under `reserved` instead so collisions become
  honest refusals (TypeScript does this for JS reserved words). Applied wherever a rule routes a
  name through `{"fn":"ident"}` — which your rules should do for **every** declared/referenced name.
- `mangle`: single-character repair for emitted names the target's grammar forbids (Python maps the
  overload marker `$` → `_`); applied via `{"fn":"mangleName"}` / the decl-context `mangle`.
- `reserved`: names your *generated* code claims (synthesized temps, scaffolding). A trailing `*`
  reserves a prefix family (`__w*`). A user identifier matching one is refused with a per-target
  diagnostic — this is what makes generated-name collisions loud instead of miscompiles.
- `globals`: runtime globals user code must not shadow (Python's `str`); same refusal mechanism.

## 4. The rule language

A rule is either a bare JSON string (a literal) or an object whose single distinguishing key names
the primitive. Rules live in the manifest's `rules` table, keyed by IR construct name; extra entries
(PHP's `phpParam`, `phpFnSig`) are helper rules reachable via `call`/`mapMembers`.

### 4.1 String-flavor primitives (produce text)

| Primitive | One-liner | Example |
|---|---|---|
| *(bare string)* | Literal text. | `"This": "$this"` |
| `tmpl` | Concatenate a list of sub-rules. | `{"tmpl": ["$", {"fn":"ident","args":[{"get":"node.name"}]}]}` |
| `get` | Read a context scalar by path. | `{"get": "node.text"}` |
| `emit` | Recursively emit the child expression at a path (no parens added). | `{"emit": "node.index"}` |
| `emitChild` | Like `emit`, with `side` (`"l"`/`"r"`/`"recv"`/`"unary"`) driving `wrapAtom` parenthesization. | `{"emitChild": "node.lhs", "side": "l"}` |
| `map` | Emit each element of a list, joined by `sep`; optional `item` template (paths on `item.…`), optional `side`. | `{"map": "node.args", "sep": ", "}` |
| `interleave` | Zip a literal list with a hole list (string interpolation): `lit0 hole0 lit1 …`. Takes `lits`, `holes`, `lit`, `hole`. | see PHP `Interp` |
| `fold` | Right-fold over a list (`list`, `each`, `seed`); `each` reads the accumulated tail as `{"get":"acc"}`. Python's match → ternary chain. | see Python `Match` |
| `fn` | Invoke a catalog builtin with `args` (each an evaluated sub-rule). Unknown names fail the **load**. | `{"fn": "wrap", "args": [{"get":"node.type"}, {"emit":"node.operand"}]}` |
| `call` | Evaluate a named helper rule from the same table (depth-capped at 64). | `{"call": "phpParam"}` |
| `type` | Render the TypeRef at a path through the plugin's `Type` rule. | `{"type": "decl.extBase"}` |
| `fresh` | Mint a single-eval temp name (`prefix` + counter), exposed under alias `as` inside `in`. | `{"fresh": {"prefix":"__w", "as":"t", "in": {...}}}` |
| `case` | Branch: `when` = list of `[test, rule]` pairs, first match wins; optional `else` (no match, no else → empty). | `{"case": {"when": [[{"eq":["node.op","-"]}, "..."]], "else": "..."}}` |

Tests inside `case`/`when`: `{"eq": [path, value]}`, `{"has": path}` (non-empty),
`{"and": [...]}`, `{"or": [...]}`, `{"not": test}`.

### 4.2 Decl-flavor primitives (write indented lines)

| Primitive | One-liner |
|---|---|
| `line` | One indented line; the payload is a string-flavor rule. |
| `block` | Open a block with `head` (string rule), run `body` decl rules one level deeper, close per `blockStyle`. |
| `mapDecl` | Run the `each` decl rule once per element of a list (paths on `item.…`). |
| `mapMembers` | Run the **named** rule (`"rule"`) once per member of a list, each against its own decl context. `{"mapMembers": "decl.methods", "rule": "MethodDecl"}` |
| `stmts` | Emit the IR statement list at a path through the shared statement walk. |
| `seq` | Run decl rules in order. |
| `indent` | Run decl rules one level deeper without a head/closer (manual joins — TS's `catch` chain). |

`case`, `call`, and `fresh` work in decl flavor too.

### 4.3 The builtin `fn` catalog (the complete, fixed list)

Expression context: **`intSuffix`** (literal suffix for a type, from `spec.intSuffix`) ·
**`escapeString`** (render a double-quoted string literal with the shared `\\ \" \n \t \r` escapes) ·
**`opSpelling`** (operator via `spec.binaryOp`, verbatim fallback) · **`ident`** (keyword-escape a
name per `identifiers.escape`) · **`mangleName`** (forbidden-character repair per
`identifiers.mangle`) · **`escape`** (`[mapName, text]` — apply a named `spec.escapes` map) ·
**`wrap`** (`[width, expr]` — the `spec.wrapInt` template) · **`table`** (`[table, key]` — a
`spec.tables` lookup, `""` if absent) · **`subst`** (`[table, key, x]` — look up a template and
substitute `$x`; identity if absent) · **`require`** (`[key]` — record a prelude key, emit nothing) ·
**`inlineBlock`** (a block lambda's body flattened onto one line).

Declaration context adds: **`generics`** (spell the generic parameter list per `spec.generics`) ·
**`where`** (the trailing bounds clauses, for `"whereClauses"` style) · **`mangle`** (as
`mangleName`, on declared names).

Type context adds: **`substExtern`** (substitute rendered type args into an extern class's
`…$0…` spelling — how `Type` rules handle `List`/`Error`/`Iterable`); `ident` also works here.

That's the whole catalog — 15 names. Any other `{"fn": …}` fails the load.

### 4.4 Which construct is which flavor

- **Expression rules** (string flavor): `Int` `Float` `Bool` `Null` `Str` `Char` `Var` `This`
  `Extern` `Call` `Member` `MethodCall` `Index` `Cond` `ListLit` `Tuple` `New` `Unary` `Cast`
  `Interp` `Binary` `MakeCase` `Match` `With` `Await` `Lambda`. Paths read `node.…` — literals
  (`node.text`, `node.value`), structure (`node.args`, `node.op`, `node.field`), and precomputed
  facts (`node.typeIsInt`, `node.typeIsFloat`, `node.typeIsInt64`, `node.castSame`,
  `node.lhsIsRecord`, `node.receiverHasIndexer`, `node.hasOpMethod`, …).
- **Statement rules** (decl flavor): only `ForStmt` and `TryStmt` — paths read `stmt.…`
  (`stmt.isRange`, `stmt.binding`, `stmt.iterable`, `stmt.body`, …). Every other statement
  (let/assign/if/while/return/throw/use/break/continue/yield) is the shared engine walking your
  spec data: `blockStyle`, `stmtEnd`, `throwKeyword`, `rethrow`, and the `localDecl`/`yield` tables.
- **Declaration rules** (decl flavor): `Program` (paths on `module.…` — `module.enums`,
  `module.functions`, `module.globals`, `module.hasEntry`, `module.entry.mangledName`) and
  `EnumDecl` `RecordDecl` `ClassDecl` `MethodDecl` `FunctionDecl` `UnionDecl` `InterfaceDecl`
  (paths on `decl.…` — `decl.name`, `decl.params`, `decl.fields`, `decl.exprBodied`, …).
- **The type rule**: `Type`, reading `type.…` (`type.name`, `type.externTemplate`) — dispatch
  extern-mapped types to `{"fn":"substExtern"}` and spell the rest (see PHP's four-line `Type`).

`Program` is the whole-file scaffold. PHP's is a `seq`: a `<?php` line, `mapMembers` over enums,
records, classes, a `mapDecl` over globals, `mapMembers` over functions, and a guarded entry call.

## 5. Validation and refusals

`loadBackend` — run at every CLI startup for each discovered plugin, and as the same validation
pipeline (`validateBackend`) by `polyglot install` — rejects, with a named error:

- a non-object artifact, a missing/empty `name`, an invalid `spec`, or `spec.name` ≠ `name`;
- a missing `rules` object, or any rule that doesn't parse — the rule grammar is **strict**: an
  object that isn't one of the primitives above is an error, not ignored;
- a missing `Program` or `Type` rule;
- a **coverage gap**: a construct from §3.2's table with no rule and no declared
  `"false"`/`"emulated"` capability stance — or a capability claiming `"native"` (or any other
  value) with no rule behind it;
- an unknown `{"fn": …}` builtin (outside the 15-name catalog);
- a dangling reference: a `{"call": …}` or `mapMembers` `"rule"` naming a rule that doesn't exist;
- loading a name that's already registered (`plugin 'x' is already loaded`).

At compile time, a capability declared `false` surfaces to the user as a §3.E refusal diagnostic on
the offending construct — the build fails loudly; no output file is written for that target. The
same applies to std members without your target's overlay arm, and to user identifiers colliding
with your `reserved`/`globals` lists. Runtime belt-and-braces: a helper-rule cycle bottoms out at
depth 64 with a visible `<call-depth-exceeded:…>` poison string instead of looping.

## 6. Testing your plugin

The project's differential-conformance pattern (PRD §5): compile one `.pg` program to your target
**and** to C# (the oracle), run both, and assert byte-identical stdout.

```
polyglot build tests/conformance/programs/arithmetic.pg --target csharp --out out
polyglot build tests/conformance/programs/arithmetic.pg --target mylang --out out
# run both and diff the stdout
```

`tests/conformance/programs/*.pg` is the 38-program suite covering the full supported surface
(including the FruitCake physics north star); `tests/conformance/run-diff.ps1` (C# vs TS) and
`run-python.ps1` (Python vs the C# oracle) show the harness shape to copy for your language's
runtime. Programs exercising a capability you declared `false` are expected to *refuse* — verify
the diagnostic appears rather than skipping the program silently. If you have no runtime available,
inspection of emitted output is the fallback (that's PHP's current state), but a runtime
differential is the real gate.

While iterating, the `pgconfig.json` `file:` dependency (§2) picks up manifest edits on the next
build with no reinstall step.

## 7. Publishing

Targets ship as npm packages wrapping the single manifest. The convention (see
`plugins/csharp/package.json`):

```json
{
  "name": "@mintplayer/polyglot-target-csharp",
  "files": ["polyglot-plugin.json"],
  "publishConfig": { "access": "public" }
}
```

`polyglot install <name>` with a bare name (no `@`, no `/`) resolves to
`@mintplayer/polyglot-target-<name>`; a full spec (`@scope/name`, `name@version`) passes through
untouched. Install shells out to `npm pack`, extracts `package/polyglot-plugin.json`, validates it,
and copies it into the user cache — so any npm-published package whose tarball carries a valid
`polyglot-plugin.json` at its root works, whatever it's named.
