# Standalone HTML documentation exporter.
#
# Merges all HTML documentation into a single file with three.js diagrams.
# `shared` tier: it links no client or server code, and it is not shipped.

add_executable(docexport app/main.cpp)
target_link_libraries(docexport PRIVATE Engine::core)
target_compile_options(docexport PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(docexport PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
