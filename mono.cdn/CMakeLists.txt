# mono.cdn - the content origin: a library plus a thin executable.
#
# repo_layout.md §11 calls this member `mono.contentdelivery/` while naming its
# program, its preset and its staged directory `cdn`. The folder is spelled to
# match the three that are checked by something, so `mono.cdn/`, the `cdn`
# target, `--preset cdn` and `just cdn` are one word rather than two.
#
# `shared` tier is the load-bearing line here, and it is stricter than the
# server's. MONO_TIER_ALLOWS_shared lists only `shared`, so this program cannot
# link `render`, `input` or `orchestration` at all - the tier check refuses the
# edge at configure time and names it. §11's "builds on a machine with no Vulkan
# SDK, no SDL and no shader compiler" is therefore a property of the build
# rather than a claim about a link line, and it needs no guard to stay true.
#
# §8 gave the destination as `core`, `parallel` and `assets`, and v0.9 widened it
# by two - which is a change to §8 rather than a drift within it, so it is
# written down here and in expected_graph.json rather than just happening.
#
# `net` arrived with the wire. An origin that serves nothing is a directory; the
# hop that carries bytes is `Engine::net`'s `http/`, and `ROADMAP.md` v0.9 said
# in advance that building it "also moves the origin's link row". It does not
# cost the bare build: `net` links `core` plus asio and Crypto++, and no part of
# it wants a window.
#
# `delivery` arrived with the client half. A compressed group is a format with
# two ends - this program builds one and a game expands one - so `GroupCodec`
# moved out of here into `Engine::delivery` where both can reach it. Keeping a
# copy on each side is how a format acquires a dialect, which CDN.md §6 refuses
# twice. Zstd went with it, which is why this file no longer names a vendor.
#
# What has *not* changed is the property the row exists for: every one of these
# is `shared`, so `cmake --preset cdn` still configures on a container with no
# Vulkan SDK, no SDL and no shader compiler. `just check-cdn-is-bare` is the
# standing proof and it is the thing to run when this line is edited again.
mono_add_library(cdn
	TIER shared
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::assets
		Engine::delivery
		Engine::net
		Engine::parallel

		# `shared`. The distribution streams of ROADMAP.md v0.13: `--advertise`
		# offers this origin on the subnet, `--stream-key` makes it private, and
		# `--rendezvous-listen` runs the meeting place other people's sessions
		# use. All three are off by default, so an origin nobody asked to be
		# findable opens no socket for any of them.
		#
		# It costs the bare build nothing, which is the line to check when this
		# row is edited: `Mono::network` links `core`, `net` and Crypto++ - a
		# UDP socket and an HMAC, and no part of it wants a window. `just
		# check-cdn-is-bare` is the standing proof.
		Mono::network

		# `shared`. What a person's Discord profile says while this program is
		# running. It links `core` and a header-only JSON parser and nothing
		# else, so this row does not put a graphics stack or a wire protocol on
		# an origin's link line - which is the property `just check-cdn-is-bare`
		# is there to keep true.
		Mono::discord
)

# The library above is built whenever the server is, because a server can attach
# an origin. The *program* is what MONO_BUILD_CDN gates, so `--preset server`
# stages no origin binary and the staged tree keeps saying what it contains.
if(MONO_BUILD_CDN)
	mono_add_program(cdn
		TIER shared
		SOURCE app/main.cpp
		DEPS
			Mono::cdn
			Engine::core
	)
endif()
