# L3 shared. Owned observations and independent checks, without runtime adapters.
mono_add_library(testing
	TIER shared
	DEPS Engine::core Engine::parallel
)
