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
)

mono_add_program(server
	TIER server
	SOURCE app/main.cpp
	DEPS
		Mono::server
		Engine::core
)
