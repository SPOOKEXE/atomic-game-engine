# L10 shared. The explicit source rigid-body host owns the private Box2D backend.
mono_add_library(imagegraphphysics
	TIER shared
	DEPS Engine::core Engine::imagegraph
	VENDOR Vendor::box2d
)
