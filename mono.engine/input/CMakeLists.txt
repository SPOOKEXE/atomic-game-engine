# L12 [client]. Presentation-side, so it is absent from the server binary
# entirely — a server-side script that reaches for input fails because the class
# was never registered into that binary, not because a check rejected it.

mono_add_library(input
	TIER client
	DEPS Engine::core

	# SDL_Event appears in the public header: this module translates events, it
	# does not own the loop that pumps them.
	VENDOR_PUBLIC SDL3::SDL3
)
