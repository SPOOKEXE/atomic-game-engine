# L8 shared. Byte decoders and sample values carry no device dependency.
mono_add_library(audiocodec
	TIER shared
	DEPS
		Engine::core
	VENDOR
		Vendor::minimp3
)
