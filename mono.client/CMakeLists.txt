# mono.client — a library plus a thin executable.
#
# The split is not cosmetic. A test binary needs something to link, and
# single-player needs the client to be able to link a server library and host
# one in-process. Neither is possible when the program is one executable's worth
# of globbed sources. repo_layout.md §2.

mono_add_library(client
	TIER client
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::parallel
		Engine::ecs
		Engine::world
		Engine::input
		Engine::render

		# L7, `shared`. The component set both programs share: the demo world is
		# built out of it, the replicated world arrives as it, and the draw list
		# a world publishes is `scene::DrawInstance`. Before v0.4 this directory
		# declared its own `Transform` and its own copy of the server's
		# components, and the two had to be kept in step by hand.
		Engine::scene

		# `--connect` makes one of this client's worlds somebody else's
		# authority. Both are `shared` tier, so neither widens what a client is
		# allowed to link — the tier rule already permits `shared` here.
		Engine::net
		Engine::replication

		# L10, `shared`. `--script` builds the scene by running a Luau file
		# instead of the C++ loop in Demo.cpp. The VM itself is private to that
		# module — nothing here compiles against Luau — so this row says "the
		# client can run a game file" and nothing about which language it is in.
		Engine::examples
	VENDOR SDL3::SDL3
)

mono_add_program(client
	TIER client
	SOURCE app/main.cpp
	DEPS
		Mono::client
		Engine::core
		Engine::render

	# Single-player will link the server library into this process, and that is
	# the one edge the tier rule has to allow by name rather than by rule:
	#
	#   DEPS ... Mono::server
	#   ALLOW_TIER_ESCAPE Mono::server   # single-player hosts in-process
	#
	# Naming it keeps it a decision rather than a precedent. Not declared yet,
	# because nothing here hosts a server — and an escape that is not currently
	# needed is an escape somebody will eventually use for something else.
	# repo_layout.md §8.
)
