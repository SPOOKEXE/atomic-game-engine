# L12. What the server tells a client, and what the client does with it.
#
# **`net` carries bytes and this decides what they mean.** `net/AGENTS.md` is
# explicit that a transport must not know what a component is — a layer that did
# could not also carry a script's remote call or a group of content bytes — so
# the knowledge of entities, tables and snapshots lives here, one layer up, and
# net stays swappable underneath it.
#
# `shared` tier, because both halves run the same code. The server derives a
# delta and the client applies it; single-player runs both in one process over a
# loopback with real encoding, so there is no configuration in which either half
# is skipped.
mono_add_library(replication
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::net
		Engine::world
)
