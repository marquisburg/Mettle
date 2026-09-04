# Abstractions the language does not have a word for

Three shapes a program needs and a language usually lacks, each built in Mettle
with no preprocessor, no plugin, no compiler fork, and no committee.

| File | What it builds | What checks it |
|------|----------------|----------------|
| `table.mettle` | A table declared once with its accessors generated | The type checker, on the program the directives expand into |
| `wire.mettle` | A wire format generated for both ends | The same, plus tags built while compiling so the two ends cannot disagree |
| `variants.mettle` | A set of variants with a completeness check | A `@rule` over the checked program, which fails the build naming the variant nobody decided |

Run them:

```bash
mettle --build examples/abstractions/table.mettle -o table.exe
mettle --build examples/abstractions/wire.mettle -o wire.exe
mettle --build examples/abstractions/variants.mettle -o variants.exe --report-rules
```

Read what they generated, as ordinary Mettle:

```bash
mettle expand examples/abstractions/wire.mettle
```

See what they cost:

```bash
mettle --build examples/abstractions/wire.mettle -o wire.exe --report-expansion
```

The point of each is the same. What a preprocessor produces is code the type
checker meets for the first time afterwards, with errors pointing at lines
nobody wrote. What these produce is checked as the program it expands into, and
carries its expansion chain into every diagnostic. `variants.mettle` carries a
`default:` arm on purpose: the compiler's own exhaustiveness check is satisfied
by it, and the rule is what refuses. That is the shape of an extension the
compiler still keeps its claims about.
