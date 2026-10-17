# L12 [client]. The mixer graph, spatialisation and the device.
#
# The layer plan puts `audio` at L12 client, beside `render`, `ui` and `input` -
# so it is absent from the server binary entirely, and a
# server-side script reaching for it fails because the class was never
# registered into that binary rather than because a check rejected it.
#
# **The graph and the DSP are still `client` tier even though they are pure
# arithmetic**, and that is the layout's decision rather than this file's. A
# server does not mix: it decides *what* is audible and replicates that, and the
# sound is produced where somebody is listening. Putting the mixer at `shared`
# to make it testable would put an audio graph in the server binary to buy a
# property the null device already gives - every test below runs with no sound
# card.
#
# SDL is VENDOR rather than VENDOR_PUBLIC: no public header here names an
# `SDL_AudioStream` or an `SDL_AudioSpec`. `Device.hpp` hands out an interface
# and a factory, which is what lets the null device and the real one be
# interchangeable at the call site - the property the whole suite depends on.
# Pure sample buffers and byte decoders are exported by the lower shared audiocodec module.
mono_add_library(audio
	TIER client
	DEPS
		Engine::core
		Engine::audiocodec
	VENDOR
		SDL3::SDL3
)
