# L12 [client]. Presentation-side, so it is absent from the server binary
# entirely - a server-side script that reaches for input fails because the class
# was never registered into that binary, not because a check rejected it.

mono_add_library(input
	TIER client
	DEPS
		Engine::core

		# **L7, `shared`, and it is a public dependency.** `Translate.hpp` writes
		# `scene::InputState` and names it in its own signature, because that is
		# the whole job: this module is where an SDL keycode becomes a name, and
		# the name lives on a resource a script can reach. `Actions.hpp`'s rule -
		# nothing outside this module names a key - is what puts the translation
		# here rather than in the client.
		Engine::scene

	# SDL_Event appears in the public header: this module translates events, it
	# does not own the loop that pumps them.
	VENDOR_PUBLIC SDL3::SDL3
)
