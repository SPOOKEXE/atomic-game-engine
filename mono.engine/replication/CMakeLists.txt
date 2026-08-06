mono_add_library(replication
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::net
		Engine::world

		Engine::assets
)
