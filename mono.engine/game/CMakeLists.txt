# L10 [shared]. What a game file is, and nothing about what reads one.
#
# **Shared rather than client**, which is the whole reason it is a module and
# not part of `mono.studio`. The studio writes a game file, a dedicated server
# reads one, and a client reads one to play single-player - three programs
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
		# against - the properties a document holds are the ones a class
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

		# L7, `shared`. The 2D class tree, for the same reason `scene` is here:
		# a game file names its classes and this module resolves the names, so
		# a tree a file can carry is a tree this target has to know about.
		Engine::gui

		# L9. `script::SourceCache` is where a document's script text lands, and
		# it is the field `script/Instances.hpp` predicted this version would
		# need.
		Engine::script

		# L11. **Because `StartWorldScripts` opens a VM.** A document says which
		# language it is in and this module acts on that, so it needs the factory
		# rather than either adapter - which is what puts this module at L12 since
		# v0.19.
		Engine::scripthost

		# L9, `shared`. The asset pipelines a world carries, as
		# `bake::PipelineSet` and its text format.
		#
		# **`bakegraph` and never `bake`**, which is the entire reason that
		# module exists. `bake` carries the PNG, JPEG, GIF, BMP, OBJ, glTF and
		# PMX readers and its own build file says nothing a shipped game links
		# may link it - so naming it here to parse a *text document* would put
		# every decoder into `server`, which links this and has no reason to
		# decode a JPEG. `bakegraph` links `core` and nothing else; the day this
		# edge wants widening to `bake`, the answer is no. `D00102`.
		Engine::bakegraph

		# A world carries its render pipelines the way it carries its scripts:
		# authored content, and a world that exported its parts but forgot how it
		# was drawn would lose an editor's work on exit. The set serialised to
		# text, so the format embedded it rather than inventing a second one.
		# `Game.cpp` still recognises the `<Pipelines>` element so those files
		# load; see the markers there.
		#
		# Drop this edge if the new system does not put a resource on the world.
		Engine::graph

		# L5 and L8, together, for one file. `CollisionContent.cpp` turns an
		# `assets::MeshData` into the hull and the triangle soup a
		# `scene::Collider` names, and this is the lowest module that can see
		# both halves: the geometry types are at L5 and cannot see an asset, the
		# components are at L7, and `assets` is at L8.
		#
		# **`assets` and never `bake`**, for exactly the reason the `bakegraph`
		# edge above gives. `assets` links `core` and two vendors and holds the
		# engine's own mesh format; `bake` holds the glTF, OBJ and PMX readers.
		# A server links this target and has no business decoding a PMX.
		#
		# **Nothing here fetches.** This module does open files - a `.agame` is
		# read and written through a path a caller named - but nothing behind a
		# content id is resolved here. Those bytes arrive already in hand: the
		# client's from `delivery`, a server's from the store it is already
		# serving, so the trust boundary stays where it was.
		Engine::collision
		Engine::assets
)
