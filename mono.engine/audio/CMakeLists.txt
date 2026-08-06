# L12 [client]. The mixer graph, spatialisation and the device.
#
# repo_layout.md §5.2 puts `audio` at L12 client, beside `render`, `text`, `ui`
# and `input` — so it is absent from the server binary entirely, and a
# server-side script reaching for it fails because the class was never
# registered into that binary rather than because a check rejected it.
#
# **The graph and the DSP are still `client` tier even though they are pure
# arithmetic**, and that is the layout's decision rather than this file's. A
# server does not mix: it decides *what* is audible and replicates that, and the
# sound is produced where somebody is listening. Putting the mixer at `shared`
# to make it testable would put an audio graph in the server binary to buy a
# property the null device already gives — every test below runs with no sound
# card.
#
# SDL is VENDOR rather than VENDOR_PUBLIC: no public header here names an
# `SDL_AudioStream` or an `SDL_AudioSpec`. `Device.hpp` hands out an interface
# and a factory, which is what lets the null device and the real one be
# interchangeable at the call site — the property the whole suite depends on.
# **`core` and nothing else, and that is deliberate.** The obvious second
# dependency is `scene`, so that a listener and an emitter could be placed by the
# same `Transform` the renderer reads. It is not here because nothing in this
# module calls it yet: `ListenerPose` and `EmitterPlacement` are plain
# coordinates, and whoever drives the mixer converts. A dependency declared ahead
# of its first caller is indistinguishable from a mistake.
#
# It is the right edge to add the day audio reads components directly, and when
# that happens rule 2 is the argument — one copy of where things are, not two.
#
# minimp3 is VENDOR for SDL's reason: no public header here names a `mp3dec_t`,
# and `Mp3.hpp` hands back the same `SampleBuffer` `Wav.hpp` does. It arrived
# with its first caller — `DecodeMp3` — rather than ahead of one, and it was
# addable at all because it is CC0. A codec is usually where a licence question
# ends the conversation, which is why `Wav.hpp` carried the gap honestly for a
# release rather than listing an extension it could not decode.
mono_add_library(audio
	TIER client
	DEPS
		Engine::core
	VENDOR
		SDL3::SDL3
		Vendor::minimp3
)
