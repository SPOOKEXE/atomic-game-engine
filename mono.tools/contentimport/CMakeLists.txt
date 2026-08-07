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
add_executable(contentimport app/main.cpp)
target_link_libraries(contentimport PRIVATE Engine::core Mono::cdn)
target_compile_options(contentimport PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(contentimport PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
