# mono.discord - what a person's Discord profile says while an Atomic program
# is running. A library and nothing else.
#
# `shared` tier, and that is forced rather than chosen: MONO_TIER_ALLOWS_server
# lists only `shared`, and the server reports presence like everything else. It
# also means this cannot link a renderer, a world or an input device, which is
# right - all four programs want the same socket for different reasons, and a
# presence layer that knew what a game was could not also describe a content
# origin.
#
# ## Why this is not `mono.engine/<something>`
#
# `mono.network/CMakeLists.txt` states the rule and this is the same case: a
# member is top-level rather than an engine module when several programs import
# it and none of them is a layer of the engine. The studio, the client, the
# server and the origin each own one of these; nothing in `mono.engine` does or
# should.
#
# ## Why this is not the Discord Social SDK
#
# The SDK Discord documents is a prebuilt closed-source shared library fetched
# from their developer portal after accepting terms. It is not on a package
# index and cannot be a submodule, so it would be either a committed binary or a
# download step that breaks the build for whoever skipped it - and every other
# entry in `mono.vendor/` is a submodule. Underneath it is a small local socket
# protocol that has been stable since 2017. This is that protocol, written here,
# which is why the suite runs on a machine with no Discord and no account.
#
# `Vendor::json` is VENDOR rather than VENDOR_PUBLIC: no public header here
# names an `nlohmann::json`. `Activity` is a struct of strings and the JSON
# exists only inside `Link.cpp`, which is what keeps a wire format from
# propagating into four programs' headers.
mono_add_library(discord
	TIER shared
	NAMESPACE Mono
	DEPS
		Engine::core
	VENDOR
		Vendor::json
)
