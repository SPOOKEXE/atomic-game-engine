# L9 [shared]. Authored images, named input snapshots and shared execution admission.
mono_add_library(imagegraph
	TIER shared
	DEPS Engine::core Engine::graphcore
	VENDOR Vendor::json
)
