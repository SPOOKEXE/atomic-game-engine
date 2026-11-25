# L3 shared. Arbitrary node graphs: model, compile, run, observe. No GPU, no UI.
mono_add_library(ang
	TIER shared
	DEPS Engine::core Engine::parallel Engine::graphcore
)
