# mono.client - a library plus a thin executable.
#
# The split is not cosmetic. A test binary needs something to link, and
# single-player needs the client to be able to link a server library and host
# one in-process. Neither is possible when the program is one executable's worth
# of globbed sources. repo_layout.md §2.

mono_add_library(client
	TIER client
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::parallel
		# The loopback MCP transport and registry. It stays closed until
		# `--mcp-port` is supplied; the feature list lives in Client.cpp.
		Engine::control
		Engine::ecs
		Engine::world
		Engine::input
		# L7, `shared`. The tree a shipped client draws its own interface from.
		# `Engine::ui` is the editor's toolkit and is deliberately absent.
		Engine::gui

		# L8, `shared`. `client/Client.hpp` includes `assets/ContentPolicy.hpp`
		# to hold the process's content policy, and `Client.cpp` names the
		# manifest and the material format. **Undeclared until v0.19**, arriving
		# through `Engine::render` and `Engine::delivery`, which is the failure
		# the `Engine::physics` row below already records happening once.
		Engine::assets

		# L9, `shared`. A render pipeline is a document before it is a graph:
		# `client/Client.hpp` and `client/Scene.hpp` both name
		# `graph::PipelineDocument` in public headers and `Scene.cpp` registers
		# the render node kinds. Undeclared until v0.19, arriving through
		# `Engine::render`.
		Engine::graph

		Engine::render

		# L7, `shared`. The component set both programs share: the demo world is
		# built out of it, the replicated world arrives as it, and the draw list
		# a world publishes is `scene::DrawInstance`. Before v0.4 this directory
		# declared its own `Transform` and its own copy of the server's
		# components, and the two had to be kept in step by hand.
		Engine::scene

		# L8, `shared`. The particle pool and the ribbon builder. A client
		# installs the three systems that run them and turns the pool into
		# `render::ParticleBatch`es - see `client/Scene.hpp`, which explains why
		# that conversion is here rather than in `effects`.
		Engine::effects

		# L8, `shared`. Named because `client/src/Scene.cpp` includes
		# `physics/Characters.hpp` and `physics/Query.hpp` and calls
		# `RegisterCharacterSystems`.
		#
		# **It linked before this line existed, which is the problem.** The
		# symbols arrived transitively through `Engine::examples → Engine::script
		# → Engine::physics`, so a client built and ran while depending on an
		# edge nobody had declared - and the day `script` stops needing physics,
		# the break lands here rather than where the change was made. Both other
		# repos document having already closed the same hole for themselves.
		Engine::physics

		# `--connect` makes one of this client's worlds somebody else's
		# authority. Both are `shared` tier, so neither widens what a client is
		# allowed to link - the tier rule already permits `shared` here.
		Engine::net
		Engine::replication

		# `shared`. Finding a server rather than being told where one is:
		# `--browse` lists what is announcing itself on the subnet and
		# `--rendezvous` reaches one that is not. Both are off by default, so a
		# single-player run opens no socket for either.
		Mono::network

		# `shared`. What a person's Discord profile says while this program is
		# running. Named here rather than reached through anything else: the
		# link is owned and pumped by `Client`, and a dependency a file
		# includes is one its build file should say out loud.
		Mono::discord

		# L9, `shared`. The VM-free half of the script layer, and the one this
		# directory compiles against: `client/Client.hpp` and
		# `client/Replicated.hpp` both name `script::Runtime` in a public header
		# because a client owns one VM per world. **`Engine::scripthost` is
		# deliberately absent** - nothing here calls `MakeRuntime`; the worlds
		# are opened through `Engine::examples` and `Engine::game`, which are
		# above the adapters and name the factory themselves. Undeclared until
		# v0.19, arriving through `Engine::examples`.
		Engine::script

		# L12, `shared`. `--script` builds the scene by running a Luau file
		# instead of the C++ loop in Demo.cpp. The VM itself is private to that
		# module - nothing here compiles against Luau - so this row says "the
		# client can run a game file" and nothing about which language it is in.
		Engine::examples

		# L12, `shared`. `--game FILE.agame` plays a universe authored in the
		# studio, single-player. **Not `Mono::server`**: loading a game file and
		# running its scripts as both roles needs the format and the VM and
		# nothing else, so the tier escape reserved below is still not declared.
		Engine::game

		# L12, `client`. The mixer graph, spatialisation and the device. A game
		# that makes no sound never opens one - `OpenDevice` answers null on a
		# machine with no output and the client runs quietly - so this is a
		# dependency rather than a start-up requirement.
		Engine::audio

		# L11, `shared`. Fetching content from an origin - a local directory, a
		# server serving its own disk, or a delivery service. `--cdn` points it
		# somewhere and the default is this machine. A game whose content ships
		# inside its game file never builds one, which is why this is a
		# dependency and not a start-up step.
		Engine::delivery

	# **Private, and `client/Actions.hpp` names `SDL_Event` anyway.** That header
	# arrived at v0.19 from `Engine::input`, whose `VENDOR_PUBLIC SDL3::SDL3`
	# already propagates SDL's include directories to everything linking this
	# library - and `Actions.hpp` is a keyboard table, so the event type in its
	# signature is the same fact `input/CMakeLists.txt` states for `Translate.hpp`.
	# If `Engine::input` ever stops being public about SDL, this line becomes
	# `VENDOR_PUBLIC` in the same commit.
	VENDOR SDL3::SDL3
)

mono_add_program(client
	TIER client
	SOURCE app/main.cpp
	DEPS
		Mono::client
		Engine::core
		Engine::render

		# `main.cpp` reads `assets/ContentPolicy.hpp` to turn a content form off
		# from the command line and `assets/LocalStore.hpp` for its default local
		# source. Both are program decisions rather than library defaults, so the
		# row is here as well as on the library.
		Engine::assets

	# Single-player will link the server library into this process, and that is
	# the one edge the tier rule has to allow by name rather than by rule:
	#
	#   DEPS ... Mono::server
	#   ALLOW_TIER_ESCAPE Mono::server   # single-player hosts in-process
	#
	# Naming it keeps it a decision rather than a precedent. Not declared yet,
	# because nothing here hosts a server - and an escape that is not currently
	# needed is an escape somebody will eventually use for something else.
	# repo_layout.md §8.
)
