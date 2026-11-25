# L9 [shared]. Authored images, named input snapshots and shared execution admission.
mono_add_library(imagegraph
	TIER shared
	# ang runs the evaluation; graphcore still compiles the authored plan.
	DEPS Engine::core Engine::graphcore Engine::ang
	VENDOR Vendor::json
)
