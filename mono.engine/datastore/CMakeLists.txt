# L12. Host-side persistence providers above the world store and transport.
mono_add_library(datastore
	TIER shared
	DEPS
		Engine::core
		Engine::net
		Engine::world
	VENDOR
		SQLite::SQLite3
)
