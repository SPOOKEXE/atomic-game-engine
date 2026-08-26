# L11. Picking a VM.
#
# **Two hundred lines of source and every scripting suite, and neither of those
# is an accident.** The source is one switch on `Language`, which has to sit
# above both adapters because it is the only thing that names both. The tests are
# here because this is the only place both VMs exist, so it is the only place a
# case can assert that they agree - and "two languages, two VMs, one binding
# surface" is a claim about both, which a suite that could reach one of them
# could not check.
#
# **Above `game` and `examples`, which is why those two moved to L12 at v0.19.**
# Both call `MakeRuntime` to start a world's scripts, so both are callers of this
# module rather than peers of it. Nothing else linked either, so the move cost
# two `layer` fields.
#
# `shared`, for `script`'s reason: a server-tier host runs game scripts and so
# does a client-tier one.
#
# Everything below `script` arrives transitively; the three named edges are the
# whole of what this module knows.
mono_add_library(scripthost
	TIER shared
	DEPS
		Engine::script
		Engine::scriptjs
		Engine::scriptluau
)
