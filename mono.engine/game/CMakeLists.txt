# L10 [shared]. What a game file is, and nothing about what reads one.
#
# **Shared rather than client**, which is the whole reason it is a module and
# not part of `mono.studio`. The studio writes a game file, a dedicated server
# reads one, and a client reads one to play single-player — three programs
# across two tiers. A format owned by one of them is a format the other two
# reimplement, and rule 6's version of that is two descriptions of one thing.
#
# Nothing here draws, listens on a socket or opens a window, and the link line
# is the proof rather than a claim: no SDL, no `net`, no `render`.

mono_add_library(game
	TIER shared
	DEPS
		Engine::core

		# L3. `ecs::Classes` is the reflection this whole format is written
		# against — the properties a document holds are the ones a class
		# declares, so adding a property to a class adds it to the save file
		# with nothing here changing. That is the reason a game file needs no
		# schema of its own.
		Engine::ecs

		# L4. A game is a universe and every world in it, so the container this
		# reads and writes is `world::Universe`.
		Engine::world

		# L7. `scene::RegisterSceneClasses` is what makes `Part` a name a
		# document can carry. Named rather than left to a caller: a reader that
		# depended on somebody else having registered the classes first would
		# fail with "no class named Part" on a perfectly good file.
		Engine::scene

		# L9. `script::SourceCache` is where a document's script text lands, and
		# it is the field `script/Instances.hpp` predicted this version would
		# need.
		Engine::script
)
