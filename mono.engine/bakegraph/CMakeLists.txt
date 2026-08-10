# L9. The words a bake pipeline is described in, and nothing that runs one.
#
# **This module exists to keep a decoder out of a save format.** `bake` carries
# every importer the engine has — PNG, JPEG, GIF, BMP, OBJ, glTF, PMX — and its
# own build file states that nothing a shipped game links may link it. A world
# that carried its asset pipelines would have made `Engine::game` name `bake` to
# parse a *text document*, which puts all of those decoders into `server`, which
# has no reason to decode a JPEG.
#
# `D00102` weighed three ways out and settled on this one. Carrying pipelines as
# opaque text the save format never parses would give up the load-time validation
# the render half already has, so the two halves of one editor would behave
# differently for no reason a user could see; putting them on the universe rather
# than the world trades a dependency question for an ownership question nobody
# has answered.
#
# **`Engine::core` and nothing else.** That is the whole point of the module and
# is the line to defend: the day this needs `assets`, ask what for — a document
# is names and numbers, and a payload is `bake`'s.
mono_add_library(bakegraph
	TIER shared
	DEPS
		Engine::core
)
