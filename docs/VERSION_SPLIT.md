# Version split

`v0.26.5-imagegraph-full` preserves the full ImageGraph checkpoint from `2ea709a9` as a broader parallel snapshot, not a successor implementation of the minimal branch.

`v0.25.0-fixes` is the fixes-only line based on `18e9028c`. Premature native and PXCX stubs were removed from this line.

`v0.26.0-imagegraph-minimal` is the completed accepted first integration, built on `v0.25.0-fixes` and independently verified. Its scope is owned 2D images for `ParticleEmitter`, `ImageLabel` and existing image consumers, with basic 2D transform and composition nodes. It excludes 3D, audio and broad simulation work.

The accepted progression is `v0.24` to `v0.25.0-fixes` to `v0.26.0-imagegraph-minimal`. `v0.26.5-imagegraph-full` is the broader preserved parallel snapshot, not a successor built on minimal. The higher branch number reserves room for incremental 2D versions. `archive/v0.25.0-before-split` preserves the original mixed work.

## Review commands

Run these from the repository root:

```sh
git show --stat --oneline archive/v0.25.0-before-split
git show --stat --oneline 2ea709a9
git diff --stat 18e9028c..v0.25.0-fixes
git diff 18e9028c..v0.25.0-fixes -- ROADMAP.md VERSION docs/retired/ROADMAP.md
git show --stat --oneline v0.26.5-imagegraph-full
git diff --stat v0.25.0-fixes..v0.26.0-imagegraph-minimal
git diff v0.25.0-fixes..v0.26.0-imagegraph-minimal -- ROADMAP.md VERSION docs/retired/ROADMAP.md
git worktree list
```

## Test results

The fixes-only full dev runner initially completed 647 suites: 643 passed and 4 failed. All four failures were subsequently resolved. The stale contract fixtures now pass focused checks: server, 255 assertions across 4 cases; CDN, 101 assertions across 1 case; launcher, 32 assertions across 2 cases. The preexisting portal solid-hat fixture passed in the full script suite, with 134 cases and 14,776 assertions, including 35 portal cases and 13,967 assertions.

Headless Vulkan passed 10 cases and 364 assertions. Architecture passed with 48 modules, 6 programs, 34 layered modules and 6 fixtures. Shadercheck passed 76 shader modules. Luau passed 71 scripts, and TypeScript checks passed. The PNG decoder now refuses inflation beyond its header before growing output storage; the complete bake suite passed 121 cases and 15,352 assertions after this independent fix. The full ImageGraph archive remains preserved for review and was not revalidated.
