# Documentation search MCP server.
#
# Reads the built HTML documentation and serves full-text search via MCP over
# stdio. A client starts this as a subprocess and speaks newline-delimited
# JSON-RPC to it.
#
# `shared` tier: it links no client or server code, and it is not shipped.

add_executable(docsearch app/main.cpp)
target_link_libraries(docsearch PRIVATE
	Engine::core
	Vendor::json)
target_compile_options(docsearch PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(docsearch PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
