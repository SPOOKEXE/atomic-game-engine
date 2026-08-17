# mono.server - a library plus a thin executable.
#
# The library is `server` tier, so the tier check refuses to let anything
# `shared` or `client` link it. The client will link it by name, once
# single-player exists, and that one edge is declared as an escape in
# mono.client/CMakeLists.txt so it stays a decision rather than a precedent.
#
# Nothing here links a `client`-tier target, which is why the staged server/
# directory has no shaders/ folder and the binary contains no renderer.

mono_add_library(server
	TIER server
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::parallel
		Engine::ecs
		Engine::world

		# L13, `shared`. The Model Context Protocol surface, opened only when
		# `--mcp-port` asks for it.
		Engine::control

		# L8, `shared`. The server is the authority, so the server is what
		# simulates: a hosted game integrates its bodies and resolves its
		# contacts here, and every client watches the result. This row was
		# missing while `Server.cpp` already called `physics::Raycast` - the
		# link came in sideways through `examples` and `game`, which is a
		# dependency that works and is not declared. Naming it changes no
		# closure and costs the headless property nothing: a broad phase and a
		# solver are arithmetic, and `just check-server-is-headless` is the
		# standing proof.
		Engine::physics

		# L7, `shared`. The component set both programs share, so a snapshot
		# this process writes resolves in a client with no translation layer.
		# `shared`, so it costs the headless property nothing: a `Transform` and
		# a `DrawInstance` are arithmetic and names, and neither needs a device.
		Engine::scene

		# L7, `shared`. **A headless server that knows what an interface is, and
		# it has to.** `StarterGui` is a template whose contents are *copied* into
		# each player's own `PlayerGui` when they spawn - `gui::ResetPlayerGui` is
		# the rule and `gui::Layer::ResetOnSpawn` is what decides whether a copy
		# survives a death. Those copies are world content: they are authored on
		# the authority and replicate to the one client that owns them, exactly as
		# a part does.
		#
		# **It costs the headless property nothing.** This module depends on
		# `Engine::core` and `Engine::ecs` and nothing else - it produces a
		# `DrawList` and a `GuiEvent`, which are data. `mono.engine/ui` is the one
		# that paints, and this process does not link it. `just
		# check-server-is-headless` is the standing proof.
		Engine::gui

		# L10, `shared`. `--game` hosts the same scene file a client loads,
		# through the same loader. A server that authored it differently would
		# disagree with its own replicas once per tick, and both sides would
		# look internally consistent while doing it.
		Engine::examples

		# `--listen` makes this process the authority for clients. Both are
		# `shared` tier, so neither costs the headless property above: a UDP
		# socket and a component delta need no graphics stack, which is why
		# `check-server-is-headless` still passes with them on the line.
		Engine::net
		Engine::replication

		# `shared`. Being findable: `--advertise` announces this server on the
		# subnet and `--rendezvous` registers it with a point, so a person can
		# join without being told an address. Both are off by default, so a
		# recipe that never asks for either opens no extra socket.
		#
		# **It costs the headless property nothing**, which is the line to check
		# when this row is edited: `Mono::network` is `shared` tier and links
		# `core`, `net` and Crypto++ - a UDP socket and an HMAC, neither of
		# which needs a device. `just check-server-is-headless` is the standing
		# proof.
		Mono::network

		# L10, `shared`. What a game file is. `--game FILE.agame` hosts a
		# universe of worlds authored in the studio; the same module the studio
		# wrote it with reads it back, which is what keeps one description of
		# the format.
		Engine::game

		# L11, `shared`. The fetch half of content delivery. `server.content-mode`
		# `relay` streams content to clients over the game link, and the routes it
		# relays come out of `delivery::RouteFetcher` - the same module a client
		# fetches with, so a relayed group and a fetched one are one artefact
		# rather than two that agree until they do not.
		#
		# **It linked transitively through `Mono::cdn` before this line existed**,
		# which is the problem: a program depending on an edge nobody declared
		# breaks where the change was not made. Naming it changes no closure and
		# costs the headless property nothing - a hash, a socket and a compressor,
		# and `just check-server-is-headless` is the standing proof.
		Engine::delivery

		# `shared`. The content origin, run in-process when `--content-store`
		# asks for it - CDN.md §6's "local store", which is the self-hosted
		# case: a small game, a LAN session, development. It is the same
		# library `mono.cdn` deploys on its own and scaled, so a client cannot
		# tell the two apart, which is the property repo_layout.md §11 wants.
		#
		# **It costs the headless property nothing**, which is the line to check
		# when this row is edited: `Mono::cdn` is `shared` tier and links
		# `core`, `assets`, `delivery`, `net` and `parallel` - a hash, a socket
		# and a compressor, none of which needs a device. `just
		# check-server-is-headless` is the standing proof.
		Mono::cdn
)

mono_add_program(server
	TIER server
	SOURCE app/main.cpp
	DEPS
		Mono::server
		Engine::core
)
