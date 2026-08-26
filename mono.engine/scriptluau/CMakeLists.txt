# L10. The Luau half of the two-VM surface.
#
# Above `script` at L9, because everything a Luau script can name - the class
# tree, the property surface, the services, the signals, the tick - is described
# there with no VM in it. What lives here is the part that meets a `lua_State *`.
#
# `shared`, for `script`'s reason: a server-tier host runs game scripts and so
# does a client-tier one.
#
# **The vendor stops here, and that is the whole point of the module.** Until
# v0.19 Luau and QuickJS were both `VENDOR` on `script`, and the rule that kept
# the three kinds of file apart was a *filename* convention enforced by a
# sixty-line CMake closure walk. A file that meets the VM now lives in a module
# that links it; one that does not lives in a module that cannot. The layer check
# says the rest.
#
# **Beside `scriptjs` rather than above or below it.** Neither adapter may name
# the other: two languages over one binding surface means neither is the real
# one, and a module that hosted the other would decide that by accident.
# `scripthost` at L11 is where the choice lives.
mono_add_library(scriptluau
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
		Vendor::luau_compiler
		Vendor::luau_vm
)
