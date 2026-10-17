# Contributing

This guide covers the local prerequisites, contribution expectations, review gate, and responsible disclosure path for this repository.

## Get started

Install a recent CMake, Ninja, C++20 compiler, Python 3, and `just`.
`clang-format` is also required for the formatting check. On Linux, install the
SDL development packages required by the window system you use.

```sh
git clone --recurse-submodules <url>
cd atomic-game-engine
just setup
just build
just run --stats
```

`just setup` initializes the pinned vendor dependencies and installs the
repository hook. All build output is disposable and lives under `.cache/`.
Without `just`:

```sh
cmake --preset dev
cmake --build .cache/build/dev
./.cache/build/dev/client/client --stats
```

[RUNNING.md](RUNNING.md) is the complete command index, including product,
test, benchmark, documentation, and integration commands.

## Make a change

Read the root `AGENTS.md`, then every module `AGENTS.md` for folders you touch.
Keep the change focused, add or update the tests that prove its behavior, and
remove code that the change replaces. Public APIs need documentation in their
headers.

Before opening a pull request, follow
[the code-quality checklist](docs/CODE_QUALITY.md). At minimum, run the checks
that cover the change and report any relevant check you could not run. A strict
local gate is:

```sh
just preset=ci check
```

Use concise pull-request descriptions: what changed, why, how it was verified,
and what remains unverified. Keep performance claims tied to the preset,
scenario, and measurement that produced them.

## AI-assisted changes

AI assistance is welcome. The contributor submitting a change remains
responsible for understanding, testing, explaining, and maintaining every line
in it. Treat architecture, untrusted input, and concurrency as review-heavy
areas, not generated boilerplate.

## Bugs and security

For a normal bug, include the command or scene, the expected and actual result,
and relevant logs or captures. For vulnerabilities or hostile-input failures,
follow [SECURITY.md](SECURITY.md) and do not open a public issue.

## Licence

Contributions are licensed under MPL-2.0.
