# mono.cdn — the content origin: a library plus a thin executable.
#
# repo_layout.md §11 calls this member `mono.contentdelivery/` while naming its
# program, its preset and its staged directory `cdn`. The folder is spelled to
# match the three that are checked by something, so `mono.cdn/`, the `cdn`
# target, `--preset cdn` and `just cdn` are one word rather than two.
#
# `shared` tier is the load-bearing line here, and it is stricter than the
# server's. MONO_TIER_ALLOWS_shared lists only `shared`, so this program cannot
# link `render`, `input` or `orchestration` at all — the tier check refuses the
# edge at configure time and names it. §11's "builds on a machine with no Vulkan
# SDK, no SDL and no shader compiler" is therefore a property of the build
# rather than a claim about a link line, and it needs no guard to stay true.
#
# §8 gives the destination: the delivery service links `core`, `parallel` and
# `assets`, and stops. All three are here now and each earned its place by being
# called — `assets` when `Gate` began opening grants, `parallel` when `Origin`
# began fanning group compression out with `Jobs::For`. Nothing beyond those
# three, and a dependency declared ahead of its first caller is a dependency
# nobody can tell from a mistake. This is the one program whose short link row is
# the point of it.

#
# Zstd is VENDOR rather than VENDOR_PUBLIC: `zstd.h` reaches no public header,
# so nothing linking Mono::cdn inherits it. `Dictionary` holds bytes and a hash
# and says nothing about what will compress against them.
mono_add_library(cdn
	TIER shared
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::assets
		Engine::parallel
	VENDOR
		Vendor::zstd
)

mono_add_program(cdn
	TIER shared
	SOURCE app/main.cpp
	DEPS
		Mono::cdn
		Engine::core
)
