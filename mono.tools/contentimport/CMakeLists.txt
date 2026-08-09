# Brings files into the local content store, repeatably.
#
# A tool rather than a shell loop, because `ROADMAP.md` v0.10 asks for a named
# list of content to be added and a loop run once leaves nothing anybody can run
# again — on another machine, or after the store is cleared.
#
# Invisible to `expected_graph.json` for `bindings`' reason: the graph check
# tracks `mono_add_library` modules and `mono_add_program` programs, and a bare
# `add_executable` is neither. So this link row is reviewed here and nowhere
# else, which is an argument for keeping it short.
#
# **`Tool::assetc` is the row v0.10 added, and it is the composition rather than
# a widening.** `--publish` bakes `raw/` into `baked/` and then publishes that,
# because a raw tree published straight through hands a client bytes no runtime
# reads. The baking cannot move into `Mono::cdn` — its short link row is the
# whole point of that member and a baker is exactly the interpretation an origin
# must not do — so it lives in the programs that can link both. This is one of
# the two; `mono.studio` is the other.
add_executable(contentimport app/main.cpp)
# **`shaderc` is here and cannot be anywhere lower.** `Tool::assetc` is
# `TIER shared` and so is `Engine::bake`; `MonoVendor.cmake` gates the whole of
# shaderc on `MONO_BUILD_CLIENT`, so neither can link it. Compiling GLSL to
# SPIR-V at publish time is the same shape as the baking argument above, one
# layer along — it lives in the program that can link both.
#
# **The consequence, decided rather than discovered:** this program no longer
# builds in a server-only configure. That is correct. A server still *carries*
# shaders — `AssetKind::Shader` is in `assets`, which is shared, and an origin
# serves those bytes as it serves a mesh it never triangulates — but it has
# nothing to compile them with and nothing to draw them on, which is what
# `render` being `TIER client` and `just check-server-is-headless` protect.
target_link_libraries(contentimport PRIVATE Engine::core Mono::cdn Tool::assetc shaderc)
target_compile_options(contentimport PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(contentimport PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
