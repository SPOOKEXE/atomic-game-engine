# L10. Source-backed foreign graph imports over the L9 archive reader and
# authored imagegraph. Neither lower module needs a sideways dependency.
mono_add_library(imagegraphio
	TIER shared
	DEPS
		Engine::bake
		Engine::imagegraph
	VENDOR
		Vendor::json
)
