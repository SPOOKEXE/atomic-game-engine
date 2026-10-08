# Version split

`v0.26-imagegraph` preserves the full ImageGraph checkpoint from `2ea709a9` for review. That preserved work is not the accepted minimal implementation.

`v0.25-fixes` is the fixes-only line based on `18e9028c`. Premature native and PXCX stubs were removed from this line.

`v0.26-imagegraph-minimal` is the accepted first integration, being built and independently verified on its own branch. Its scope is owned 2D images for `ParticleEmitter`, `ImageLabel` and existing image consumers, with basic 2D transform and composition nodes. It excludes 3D, audio and broad simulation work.

## Review commands

Run these from the repository root:

```sh
git show --stat --oneline 2ea709a9
git diff --stat 18e9028c..v0.25-fixes
git diff 18e9028c..v0.25-fixes -- ROADMAP.md VERSION docs/retired/ROADMAP.md
git diff --stat 2ea709a9..v0.26-imagegraph
git diff 2ea709a9..v0.26-imagegraph -- ROADMAP.md docs/retired/ROADMAP.md
git worktree list
```

## Test results

The fixes-only full dev runner initially completed 647 suites: 643 passed and 4 failed. All four failures were subsequently resolved. The stale contract fixtures now pass focused checks: server, 255 assertions across 4 cases; CDN, 101 assertions across 1 case; launcher, 32 assertions across 2 cases. The preexisting portal solid-hat fixture passed in the full script suite, with 134 cases and 14,776 assertions, including 35 portal cases and 13,967 assertions.

Headless Vulkan passed 10 cases and 364 assertions. Architecture passed with 48 modules, 6 programs, 34 layered modules and 6 fixtures. Shadercheck passed 76 shader modules. Luau passed 71 scripts, and TypeScript checks passed. The full ImageGraph archive remains preserved for review and was not revalidated.
