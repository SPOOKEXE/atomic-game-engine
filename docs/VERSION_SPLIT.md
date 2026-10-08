# Version split

`v0.26-imagegraph` preserves the full ImageGraph checkpoint from `2ea709a9` for review. That preserved work is not the accepted minimal implementation.

`v0.25-fixes` is the fixes-only line based on `18e9028c`. Premature native and PXCX stubs were removed from this line.

`v0.26-imagegraph-minimal` is the completed accepted first integration, built on the fixes branch and independently verified. Its scope is owned 2D images for `ParticleEmitter`, `ImageLabel` and existing image consumers, with basic 2D transform and composition nodes. It excludes 3D, audio and broad simulation work.

## Review commands

Run these from the repository root:

```sh
git show --stat --oneline 2ea709a9
git diff --stat 18e9028c..v0.25-fixes
git diff 18e9028c..v0.25-fixes -- ROADMAP.md VERSION docs/retired/ROADMAP.md
git diff --stat 2ea709a9..v0.26-imagegraph
git diff 2ea709a9..v0.26-imagegraph -- ROADMAP.md docs/retired/ROADMAP.md
git diff --stat v0.25-fixes..v0.26-imagegraph-minimal
git worktree list
```

## Test results

The fixes-only full dev runner initially completed 647 suites: 643 passed and 4 failed. All four failures were subsequently resolved. The stale contract fixtures now pass focused checks: server, 255 assertions across 4 cases; CDN, 101 assertions across 1 case; launcher, 32 assertions across 2 cases. The preexisting portal solid-hat fixture passed in the full script suite, with 134 cases and 14,776 assertions, including 35 portal cases and 13,967 assertions.

Headless Vulkan passed 10 cases and 364 assertions. Architecture passed with 48 modules, 6 programs, 34 layered modules and 6 fixtures. Shadercheck passed 76 shader modules. Luau passed 71 scripts, and TypeScript checks passed. The PNG decoder now refuses inflation beyond its header before growing output storage; the complete bake suite passed 121 cases and 15,352 assertions after this independent fix. The full ImageGraph archive remains preserved for review and was not revalidated.

## Minimal 2D verification

The complete dev build and server-only build pass. The optimized unity build of assetc and the CPU evaluation benchmark pass. New engine tests pass 419 assertions in 13 cases, including an AddressSanitizer and UndefinedBehaviorSanitizer run with leak detection. Complete affected suites pass: Studio, 13,020 assertions in 662 cases; bake, 15,425 in 128; assetc, 319 in 40; nodegraph, 204 in 23. All five MCP contract suites pass 2,135 assertions across 22 cases. Headless Vulkan texture and particle suites pass 364 assertions in 10 cases. The end-to-end Composer export, signed publication and ordinary client ImageLabel/ParticleEmitter rendering test passes 23 assertions; it checks both visible consumers independently. Architecture passes with 49 modules, 6 programs, 35 layered modules and all six fixtures; the server graph passes independently. Shader checks pass 76 modules. Source checks, generated module pages and scoped formatting pass.

The full repository test runner was not repeated on the minimal branch; its unchanged fixes baseline was checked separately above. No interactive Studio session was run. The full archive was not revalidated. See [Static 2D ImageGraph](IMAGEGRAPH_2D.md) for Studio controls, the project format and the verified CLI command.
