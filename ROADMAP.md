
# ROADMAP

## Editing

Deferred items are for items that need significant systems we cannot do now.
If you can do the item now, do NOT add to the deferred list.

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. If a TODO item is not FULLY completed, split the
TODO item, keeping the concise short dash point list with block infront and
split it as another item under the same version.

For example, if we complete A and B but not C and D:
```
### v0.5
- [x] do: A1, A3, A5
- [_] do: A, B, C, D
- [x] do: G, H, I
- [x] do: K, K2, K3
```

becomes:
```
### v0.5
- [x] do: A1, A3, A5
- [x] do: A, B
- [x] do: G, H, I
- [x] do: K, K2, K3
- [_] do: C, D
- [_] deferred `D0001` for later.
```

or defer to another version.

## VERSIONS

### v0.26.1

- [x] optimise engine build process and improve caching and filesystem and disk usage
- [x] optimise test utilisation during development process by isolating fast and slow tests to different iteration stages
- [x] combine many tests that use the same scenarios into one test file that checks all of them together
- [x] rebuild fundamental test computation properly so it utilises hooks and per-scenario setups for testing many features together in one scene (extension of previous "combine many tests that use the same scenario")
- [x] rerun code format and code documentation tooling and correct issues
- [x] polish documentation layout

- [_] Implementation and validation record: [v0.26.1 implementation plan](docs/future-work/V0261_IMPLEMENTATION_PLAN.md).
- unified graph library in underlying engine
- system-specific defined graphs and nodes
- system-specific runtimes
- C01, C02, C04, C06
- All ray (marching) optimisations across current and new systems.
- Benchmarks for simple and complex scenes for ray optimisations to prove.
