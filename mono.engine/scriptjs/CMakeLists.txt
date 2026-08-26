# L10. The JavaScript half of the two-VM surface.
#
# `scriptluau`'s twin at the same height and with the same shape: `script` at L9
# says what a script may name, and this module is the part that meets a
# `JSContext *`. Neither adapter names the other.
#
# `shared`, for `script`'s reason.
mono_add_library(scriptjs
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::gui
		Engine::physics
		Engine::scene
		Engine::script
		Engine::world
	VENDOR
		Vendor::quickjs

		# For `SourceMap.cpp` and nothing else. A `.js.map` is JSON, and the
		# alternative was scanning for two known keys in a generated file -
		# cheaper to write and a parser either way, only an unreviewed one.
		#
		# **Here rather than on `script` since v0.19.** A source map is a
		# JavaScript artefact and `MapStackFrames` has exactly one caller, so the
		# edge belongs to the adapter that produced the stack. That is what left
		# `script` with no vendor at all.
		Vendor::json
)
