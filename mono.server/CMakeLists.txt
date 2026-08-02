# mono.server — a library plus a thin executable.
#
# The library is `server` tier, so the tier check refuses to let anything
# `shared` or `client` link it. The client will link it by name, once
# single-player exists, and that one edge is declared as an escape in
# mono.client/CMakeLists.txt so it stays a decision rather than a precedent.
#
# Nothing here links a `client`-tier target, which is why the staged server/
# directory has no shaders/ folder and the binary contains no renderer.

mono_add_library(server
	TIER server
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::parallel
		Engine::ecs
		Engine::world

		# `--listen` makes this process the authority for clients. Both are
		# `shared` tier, so neither costs the headless property above: a UDP
		# socket and a component delta need no graphics stack, which is why
		# `check-server-is-headless` still passes with them on the line.
		Engine::net
		Engine::replication
)

mono_add_program(server
	TIER server
	SOURCE app/main.cpp
	DEPS
		Mono::server
		Engine::core
)
