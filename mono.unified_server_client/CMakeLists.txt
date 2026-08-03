# mono.unified_server_client — both halves of replication in one process.
#
# A diagnostic product, not a shipping one: it hosts the server's placeholder
# world and the client's replicated world in one address space and hands the
# authority's messages straight to the replica, so a blank scene can be blamed
# on a stage rather than on "the network". `include/unified/Harness.hpp` says
# what it does and does not prove.
#
# **This declares the tier escape `D00008` reserved, and it is the first real
# user of it.** A `client`-tier target may not link `Mono::server`; this one
# must, because a program that hosts a server and draws its world in one process
# is precisely the arrangement the rule exists to make deliberate. D00008's
# objection was that an escape with no user is an escape somebody repurposes —
# this one has a user, and it is this line. Single-player is still not it: that
# edge belongs in `mono.client/CMakeLists.txt` and is still not declared there.
#
# The library exists for the same reason `mono.client`'s does — a test binary
# needs something to link, and `tests/` here drives the harness directly.

mono_add_library(unified_server_client
	TIER client
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::ecs
		Engine::parallel
		Engine::scene
		Engine::examples
		Engine::replication

		# The seam under test. `BuildReplicatedWorld` and
		# `RecordReplicatedTick` are the functions `--connect` runs, so this
		# reports what a real client would draw rather than an imitation of it.
		# It drags `Engine::render` and SDL onto the link line, which is a real
		# cost and is the price of not writing a second draw pass.
		Mono::client

		# The world being replicated. `server` tier, from a `client`-tier
		# target: the escape below.
		Mono::server
	ALLOW_TIER_ESCAPE Mono::server
)

mono_add_program(unified_server_client
	TIER client
	SOURCE app/main.cpp
	DEPS
		Mono::unified_server_client
		Engine::core
		Engine::scene
)
