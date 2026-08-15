#!/usr/bin/env bash
#
# Materialise a patched vendor into `.cache/vendor/<name>/` and print its path.
#
# **The contract is a directory name.** A vendor is patched if and only if
# `mono.vendor/patches/<name>/` exists and holds at least one `.patch`. There is
# no list to keep in sync and no config file: the patches say which submodule
# they belong to by where they live, and this script turns that into a tree to
# build from. Recipes that build a patched vendor call this and use what it
# prints; recipes that build an unpatched one use `mono.vendor/<name>` directly.
#
# **The submodule is never written to, and that is the whole point.** Applying a
# patch in place left `mono.vendor/<name>` permanently dirty - `git status` in
# the superproject always had a modified submodule in it, and a `git submodule
# update` either reverted the patch under a built binary or refused to run. The
# copy has neither problem: it is derived, it lives under `.cache/` with every
# other derived thing, and the submodule stays exactly at its pinned commit.
#
# **The copy is `git archive`, not `cp`.** Archiving the pinned commit means the
# tree is that commit by construction rather than whatever happens to be sitting
# in the working directory - so a hand-edit in the submodule cannot leak into a
# build, and patches always apply forward against a clean preimage. It also
# means there is no `.git` in the copy and no reverse-apply check to write: the
# tree is either fresh or untouched, never half-patched.
#
# Nested submodules are archived the same way, one per line of
# `git submodule status --recursive`. luau-lsp brings its own Luau that way.
set -euo pipefail

name=${1:?usage: vendor-tree.sh <vendor-name>}

root=$(git rev-parse --show-toplevel)
cd "$root"

src="mono.vendor/$name"
patchdir="mono.vendor/patches/$name"
dest=".cache/vendor/$name"

if [ ! -d "$src" ]; then
    echo "no such vendor: $src" >&2
    exit 1
fi

shopt -s nullglob
patches=("$patchdir"/*.patch)
if [ ${#patches[@]} -eq 0 ]; then
    echo "$patchdir holds no .patch, so $name is not a patched vendor." >&2
    echo "Build it from $src directly, or add the patch that makes it one." >&2
    exit 1
fi

git submodule update --init --recursive --checkout --depth 1 -- "$src" >&2

# **The enforcement half of the contract.** Nothing writes to a patched
# submodule any more, so anything found there is a hand-edit that no clean clone
# would reproduce - and it would be invisible in the build, because the copy
# below comes from the commit rather than from the files.
if ! git -C "$src" diff --quiet || ! git -C "$src" diff --cached --quiet; then
    echo "" >&2
    echo "$src has local edits, and a patched vendor must stay pristine:" >&2
    git -C "$src" status --short >&2
    echo "" >&2
    echo "The build works from $dest, which is archived from the pinned commit," >&2
    echo "so these edits change nothing and will be lost. Move them into a patch:" >&2
    echo "" >&2
    echo "    git -C $src diff > $patchdir/<what-it-does>.patch" >&2
    echo "    git -C $src checkout -- ." >&2
    echo "" >&2
    echo "then give the patch a preamble like the ones beside it. mono.vendor/AGENTS.md" >&2
    echo "says what a patch owes the reader." >&2
    exit 1
fi

# **What makes the tree stale, and nothing else.** The pinned commit of the
# vendor and each of its nested submodules, plus the name and content of every
# patch. A match skips the extraction entirely, so an ordinary run costs nothing
# and the build below it is a no-op.
stamp=$(
    {
        git -C "$src" rev-parse HEAD
        git -C "$src" submodule status --recursive
        for patch in "${patches[@]}"; do
            echo "$patch"
            cat "$patch"
        done
    } | sha256sum | cut -d' ' -f1
)

if [ -f "$dest/.stamp" ] && [ "$(cat "$dest/.stamp")" = "$stamp" ]; then
    echo "$name: $dest up to date" >&2
    echo "$dest"
    exit 0
fi

echo "$name: extracting $(git -C "$src" rev-parse --short HEAD) into $dest" >&2
rm -rf "$dest"
mkdir -p "$dest"
git -C "$src" archive HEAD | tar -x -C "$dest"

while read -r _ subpath _; do
    [ -n "$subpath" ] || continue
    mkdir -p "$dest/$subpath"
    git -C "$src/$subpath" archive HEAD | tar -x -C "$dest/$subpath"
    echo "$name: extracting $subpath" >&2
done < <(git -C "$src" submodule status --recursive)

# **Run from the repository root with `--directory=`, and the alternative is a
# silent no-op.** `git apply` from inside `$dest` finds this repository's `.git`,
# resolves the patch's `src/Workspace.cpp` against the *superproject* root,
# decides it is outside the current directory, and ignores it - reporting
# success while changing nothing. The form below keeps the paths inside the
# working directory git is reasoning about, so a hunk that no longer fits is an
# error rather than a language server built without its patch.
for patch in "${patches[@]}"; do
    if ! git apply --directory="$dest" "$patch"; then
        echo "" >&2
        echo "$patch does not apply to $src at $(git -C "$src" rev-parse --short HEAD)." >&2
        echo "" >&2
        echo "Upstream moved the code it edits. Read the preamble at the top of the" >&2
        echo "patch - it says what the hunk is for and where to re-point it - then" >&2
        echo "regenerate it against a checkout of that commit. Do not skip it: the" >&2
        echo "recipe that asked for this tree needs the patch to be in it." >&2
        rm -rf "$dest"
        exit 1
    fi
    echo "$name: applied $(basename "$patch")" >&2
done

# **Stamped to now, and leaving them alone is a wrong build rather than a slow
# one.** `tar` takes its mtimes from the archive and `git archive` writes the
# *commit's* date, which for a vendor bump is months before the object files
# already sitting in the build tree - so ninja reads a whole new upstream release
# as "nothing to do" and relinks the previous one's objects against it. That was
# observed rather than reasoned about: bumping Luau 0.731 to 0.734 rebuilt three
# files and produced a binary out of two versions.
#
# Extraction only happens when the stamp moved, and the stamp moving means the
# sources really are different, so the full rebuild this forces is the honest
# cost of the change rather than waste.
find "$dest" -exec touch {} +

# Last, so an interrupted run leaves a tree that is rebuilt rather than trusted.
echo "$stamp" > "$dest/.stamp"

echo "$dest"
