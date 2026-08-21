# The stdio end of the control protocol.
#
# **A byte pump with no opinions**, which is why it links `core` for argument
# parsing and asio for a socket and nothing else. It does not link the studio,
# does not parse JSON, and has no idea what a world is - the whole protocol
# lives in `mono.studio`, and this exists because MCP clients speak stdio to a
# subprocess while the editor is a window that outlives them.
#
# A bare `add_executable`, the same shape as `bindings` and `scriptcheck`, and
# invisible to `expected_graph.json` for the same reason.
add_executable(mcpbridge app/main.cpp)
target_link_libraries(mcpbridge PRIVATE
	Engine::core
	Vendor::asio)

# **The header without the library, deliberately.** All this needs from `control`
# is `DEFAULT_PORT`, and that number existing in one place is the whole point of
# it - three copies is how the editor came to advertise a port the bridge did not
# dial. Linking the module would drag `ecs` and `world` into a byte pump that has
# no idea what a world is, so it takes the include path and no symbols.
target_include_directories(mcpbridge PRIVATE
	"${CMAKE_SOURCE_DIR}/mono.engine/control/include")
target_compile_options(mcpbridge PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(mcpbridge PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
