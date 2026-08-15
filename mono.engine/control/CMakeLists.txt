# L13. Being asked what this program is doing, and told what to do about it.
#
# **The Model Context Protocol surface, shared by every program that has one.**
# The editor, the server, the client, the unified harness and the content origin
# all answer the same handshake; what differs is which tools each registers.
# Before this was a module the protocol lived in `mono.studio`, and adding it to
# a second program would have meant a second copy of a JSON-RPC implementation -
# which is the duplication the root `AGENTS.md` calls the most expensive kind.
#
# `shared`, and it has to be: a headless server exposes its worlds through this
# and links no client code at all.
#
# Above `world` because a tool's whole vocabulary is worlds and instances, and
# below anything that presents - this module builds an answer and never draws
# one.
#
# **Nothing links it by default.** Each program opts in by constructing a
# `Surface`, and each opens its port only when told to. `SECURITY.md` carries
# why that is a decision rather than a default.
mono_add_library(control
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::world
	VENDOR
		# The socket. Private: `Server.hpp` keeps asio behind a pimpl, so nothing
		# that links this module compiles a networking library.
		Vendor::asio
	VENDOR_PUBLIC
		# **Public, and it is the narrowest widening available.** `Surface.hpp`
		# declares a tool as a function returning `nlohmann::json`, so the name
		# has to be visible to every program that registers one. What it costs is
		# `json_fwd.hpp` - a forward declaration and no parser; only this
		# module's own sources include the real header.
		Vendor::json
)
