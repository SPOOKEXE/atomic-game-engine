# L11. Getting content from wherever it is to whoever asked for it.
#
# **Why this is a module and not part of `assets`.** repo_layout.md §5.2 lists
# "remote fetch" under `assets` at L8, and that cannot be built: fetching needs
# a transport, the transport is `net` at L11, and rule 1 says a layer may see
# every layer below it and none above it. `assets/AGENTS.md` reaches the same
# conclusion from the other side — *there is no network in this module and there
# will not be* — so the fetch path has to sit above both. It does, here, and
# `assets` stays the format.
#
# **Why it is not part of `mono.cdn` either.** The origin is a product; this is
# the half a game links. An engine module depending on a program's library is
# the wrong direction, and duplicating the group format into both ends is how a
# format acquires a dialect — which is the one thing CDN.md §6 refuses twice.
#
# So the split is: `assets` is what content *is*, this is what a group is *in
# flight* and how one is fetched, and `mono.cdn` is the origin's policy, its
# admission gate and its serving. `mono.cdn` links this rather than the reverse.
#
# `shared` tier, because both halves run it: a client fetches with this and the
# origin compresses with the same `GroupCodec`, so there is one definition of a
# compressed group rather than two that agree until they do not.
#
# Zstd is VENDOR rather than VENDOR_PUBLIC: `zstd.h` reaches no public header
# here, so nothing linking Engine::delivery inherits it. `Dictionary` holds
# bytes and a hash and says nothing about what will compress against them.
mono_add_library(delivery
	TIER shared
	DEPS
		Engine::core
		Engine::assets
		Engine::net
	VENDOR
		Vendor::zstd
)
