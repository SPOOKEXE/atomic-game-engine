# mono.unified_tests - every arrangement of this engine's halves in one process.
#
# A diagnostic product, not a shipping one. It hosts the server's world and the
# client's replicated world in one address space, with the middle being whatever
# `unified::Arrangement` says - nothing at all, a real `net` link, a link that
# loses datagrams - and with `cdn` content and `network` discovery alongside.
# `include/unified/Crossing.hpp` says what it does and does not prove, and
# `include/unified/Reports.hpp` says why it holds every other module's report.
#
# **This declares the tier escape `D00008` reserved, and it is the first real
# user of it.** A `client`-tier target may not link `Mono::server`; this one
# must, because a program that hosts a server and draws its world in one process
# is precisely the arrangement the rule exists to make deliberate. D00008's
# objection was that an escape with no user is an escape somebody repurposes -
# this one has a user, and it is this line. Single-player is still not it: that
# edge belongs in `mono.client/CMakeLists.txt` and is still not declared there.
#
# **Everything below arrives transitively through the two products above, and is
# named anyway.** A module that includes `<cdn/Publisher.hpp>` and does not
# declare `Mono::cdn` compiles until the day `mono.client` stops linking it, at
# which point the break is in a file that never changed. The link line is the
# same either way; what is different is whether it says so.
#
# The library exists for the same reason `mono.client`'s does - a test binary
# needs something to link, and `tests/` here drives the crossings directly.

mono_add_library(unified_tests
	TIER client
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::ecs
		Engine::parallel
		Engine::scene
		Engine::examples
		Engine::replication

		# The wire, for the arrangements that have one: loopback transports, a
		# real handshake, `Packet` framing and `LossyTransport`.
		Engine::net

		# The publication a relayed route is fetched out of, and the key that
		# signs its manifest.
		Engine::assets
		Engine::delivery

		# The seam under test. `BuildReplicatedWorld` and
		# `RecordReplicatedTick` are the functions `--connect` runs, so this
		# reports what a real client would draw rather than an imitation of it.
		# It drags `Engine::render` and SDL onto the link line, which is a real
		# cost and is the price of not writing a second draw pass.
		Mono::client

		# The world being replicated, and the relay that answers a client's
		# route requests out of this host's own content. `server` tier, from a
		# `client`-tier target: the escape below.
		Mono::server

		# Where the content behind that relay comes from.
		Mono::cdn

		# The beacon and the directory, for the discovery axis.
		Mono::network
	ALLOW_TIER_ESCAPE Mono::server
)

mono_add_program(unified_tests
	TIER client
	SOURCE app/main.cpp
	DEPS
		Mono::unified_tests
		Engine::core
		Engine::scene
)
