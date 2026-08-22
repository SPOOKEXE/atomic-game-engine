# L11 [client]. SPIR-V to MSL, for the two callers that need it at two times.
#
# **`Engine::core` and nothing else, which is the point.** It takes words and
# hands back a string, so it sits below everything that has an opinion about
# shaders and can be linked by `mono.tools/shadercross` - a build step that must
# not drag the renderer in - and by `Engine::render`, which needs the same
# translation while the engine runs. One implementation is what stops a
# `ShaderScript` and a built-in shader from landing on different Metal indices.
#
# **`core` arrived at v0.19 for the log and for nothing else.** The index
# assignment below is the whole module and its failure is invisible: a texture
# on the wrong `[[texture(n)]]` samples somebody else's map on macOS and is
# correct on Linux. Both callers already link `core` - `shadercross` names it
# directly - so the closure of every program that reaches here is unchanged, and
# the property this row actually protects is that nothing here has an opinion
# about shaders, which still holds.
#
# `client`, for `Engine::resources`' reason rather than by analogy: SPIRV-Cross
# is configured only when `MONO_BUILD_CLIENT` is on, so a `shared` module linking
# it would break the `server` preset.
#
# The vendor is private. Nothing about SPIRV-Cross reaches a public header, which
# is why `Translation` is a string and a bool.

mono_add_library(msl
	TIER client
	DEPS Engine::core
	VENDOR Vendor::spirv-cross
)
