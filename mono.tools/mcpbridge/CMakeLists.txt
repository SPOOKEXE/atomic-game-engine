# The stdio end of the control protocol.
#
# **A byte pump with no opinions**, which is why it links `core` for argument
# parsing and asio for a socket and nothing else. It does not link the studio,
# does not parse JSON, and has no idea what a world is — the whole protocol
# lives in `mono.studio`, and this exists because MCP clients speak stdio to a
# subprocess while the editor is a window that outlives them.
#
# A bare `add_executable`, the same shape as `bindings` and `scriptcheck`, and
# invisible to `expected_graph.json` for the same reason.
add_executable(mcpbridge app/main.cpp)
target_link_libraries(mcpbridge PRIVATE
	Engine::core
	Vendor::asio)
target_compile_options(mcpbridge PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(mcpbridge PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
