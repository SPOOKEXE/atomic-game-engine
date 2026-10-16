# L3 shared. Typed buffer contracts and plans; hosts own GPU execution.
mono_add_library(computegraph
	TIER shared
	DEPS Engine::core Engine::graphcore
)
