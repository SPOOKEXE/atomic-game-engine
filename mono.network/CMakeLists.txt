# mono.network — finding a peer, and being findable. A library and nothing else.
#
# `shared` tier, and that is the load-bearing line. MONO_TIER_ALLOWS_shared
# lists only `shared`, so this cannot link a renderer, an input device or a
# world — and it must not, because the three programs that import it want the
# same answers for different reasons. A discovery layer that knew what a game
# was could not also list a content origin.
#
# **A library, never a program.** `mono.engine` has that rule because it is the
# engine; this member has it because a rendezvous point is not a product of its
# own — `mono.cdn` hosts one with `--rendezvous-listen`, and a second executable
# doing the same thing would be a second deployment to configure and a second
# place for the message set to drift.
#
# ## Why this is not `mono.engine/net`
#
# `Engine::net` is the wire between two machines that already know where each
# other are. This is everything before that: an announcement on a subnet, a
# registration at a rendezvous point, a hole punched through two NATs, and the
# pre-shared key that decides whether a stranger is allowed to try. The two
# answer different questions and only one of them is on the hot path.
#
# It is a top-level member rather than an engine module for the reason
# `mono.cdn` is: the studio and the content origin import it, and neither of
# them is a layer of the engine.
#
# ## Why it does not link `replication`
#
# Because `replication::Listener` and `replication::Connector` already own a
# connected session over a transport, and two ways to do one job is the most
# expensive debt in a monorepo. What this member produces is an
# `engine::net::Endpoint` — the argument `Connector` takes, the address a
# `delivery::Source` names, the host an `http::Client` dials. Where that
# endpoint came from is this module's business and nobody else's, which is what
# makes LAN, peer-to-peer and remote one code path at every call site.
#
# cryptopp is VENDOR rather than VENDOR_PUBLIC, for the reason `net` gives: no
# public header here names an HMAC, and `SessionKey` is 32 bytes behind a class
# that wipes itself.
mono_add_library(network
	TIER shared
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::net
	VENDOR
		Vendor::cryptopp
)
