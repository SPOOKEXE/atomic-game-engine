# L3 shared. Typed buffer contracts and plans; hosts own GPU execution.
mono_add_library(computegraph
	TIER shared
	# ang orders the passes and holds the payload ceiling as a budget.
	DEPS Engine::core Engine::graphcore Engine::ang
)
