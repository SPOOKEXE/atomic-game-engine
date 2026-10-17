# L8 [shared]. grug bound image bytes before runtime or bake owns pixels.
mono_add_library(imagecodec
	TIER shared
	DEPS Engine::core
	VENDOR Vendor::cryptopp
)
