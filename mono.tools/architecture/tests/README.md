# Fixtures for the architecture check

`CheckTargetGraph.cmake` is the only check in the repository that can pass by
doing nothing: it walks an expectation, and an expectation it fails to parse
walks as zero entries and reports success. So the check is checked.

Each directory here is a three-file set: a `graph.json` standing in for what
CMake emitted, an `expected.json` standing in for the checked-in expectation,
and an `expect` file naming the message the pair must produce. `just
test-architecture` runs the real graph first and then every fixture, and **a
fixture that passes when its `expect` is non-empty is the bug.**

Each set is self-contained rather than derived from a shared template, so a
fixture can be read on its own and changing one cannot move another.

| Fixture | What it breaks |
|---|---|
| `upward` | a module links something at a higher layer |
| `lateral` | a module links a sibling at its own layer, unnamed |
| `program-band` | a module links a program-band entry, which has no layer |
| `unnamed-module` | the build declares a module the expectation does not |
| `wrong-tier` | the build's tier disagrees with the expectation |
| `clean` | nothing. `expect` is empty, so this pair must **pass** - without it, a check that failed on everything would go green here |
