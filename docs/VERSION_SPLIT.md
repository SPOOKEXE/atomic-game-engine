# Version split

`v0.26-imagegraph` preserves the full ImageGraph checkpoint from `2ea709a9` for review. That preserved work is not the accepted minimal implementation.

`v0.25-fixes` is the fixes-only line based on `18e9028c`. Premature native and PXCX stubs were removed from this line.

The first v0.26 integration remains for later user review. Its proposed scope is owned 2D images usable by `ParticleEmitter`, `ImageLabel` and existing image consumers, with basic 2D transform and composition nodes. It excludes 3D, audio and broad simulation work.

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

Pending root verification.
