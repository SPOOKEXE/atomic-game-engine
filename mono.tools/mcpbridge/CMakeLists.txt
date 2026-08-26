# The stdio end of the control protocol.
#
# **A byte pump with no opinions**, which is why it links `core` for argument
# parsing and asio for a socket and nothing else. It does not link the studio,
# does not parse JSON, and has no idea what a world is. The whole protocol
# lives in `Engine::control`, and this exists because MCP clients speak stdio to a
# subprocess while each engine product is a process that outlives them.
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
# dial. Linking the module is still unnecessary for a byte pump with no protocol
# work, so it takes the include path and no symbols.
target_include_directories(mcpbridge PRIVATE
	"${CMAKE_SOURCE_DIR}/mono.engine/control/include")
target_compile_options(mcpbridge PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(mcpbridge PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")

# The port, checked rather than trusted, in the three places outside C++ that
# name it.
#
# **`engine::control::DEFAULT_PORT` fixed the copies the compiler can see and not
# the ones it cannot.** A client is pointed at a port by `.mcp.json`, by the
# `just mcp` recipe and by the header - and it was the first two that stayed
# right by somebody remembering to edit them. `resources/CMakeLists.txt` reads a
# constant out of a header for the same reason and this is the same trade: rule 6
# says a rule the build does not check is documentation, and "these four numbers
# agree" was documentation until this ran.
#
# Read at configure time, so an edit to any of them is an error the next
# `cmake --preset` reports with the disagreeing values named.
set(bridge_header "${CMAKE_SOURCE_DIR}/mono.engine/control/include/engine/control/Server.hpp")
file(STRINGS "${bridge_header}" bridge_port_line
	REGEX "inline constexpr uint16_t DEFAULT_PORT")
if(NOT bridge_port_line MATCHES "=[ \t]*([0-9]+)")
	message(FATAL_ERROR
		"mcpbridge: could not read DEFAULT_PORT from ${bridge_header}.\n"
		"  `.mcp.json` and the `just mcp` recipe are checked against it. If that "
		"declaration moved or changed shape, update this regex in the same commit.")
endif()
set(bridge_port "${CMAKE_MATCH_1}")

set(bridge_elsewhere
	"${CMAKE_SOURCE_DIR}/.mcp.json"
	"${CMAKE_SOURCE_DIR}/Justfile")

foreach(bridge_file IN LISTS bridge_elsewhere)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bridge_file}")
endforeach()

file(STRINGS "${CMAKE_SOURCE_DIR}/.mcp.json" bridge_mcp_json
	REGEX "\"--port\"")
if(NOT bridge_mcp_json MATCHES "\"([0-9]+)\"[^\"]*$")
	message(FATAL_ERROR
		"mcpbridge: .mcp.json no longer names a --port. It is what a client reads "
		"to find the editor, so it has to name one.")
elseif(NOT CMAKE_MATCH_1 STREQUAL bridge_port)
	message(FATAL_ERROR
		"mcpbridge: .mcp.json dials port ${CMAKE_MATCH_1} and "
		"engine::control::DEFAULT_PORT is ${bridge_port}.\n"
		"  A client that reads .mcp.json would connect to a port the editor does "
		"not open. Change both, in the same commit.")
endif()

file(STRINGS "${CMAKE_SOURCE_DIR}/Justfile" bridge_recipe
	REGEX "^mcp port=")
if(NOT bridge_recipe MATCHES "port=\"([0-9]+)\"")
	message(FATAL_ERROR
		"mcpbridge: the `just mcp` recipe no longer has a default port. It is what "
		"opens the editor's surface, so it has to have one.")
elseif(NOT CMAKE_MATCH_1 STREQUAL bridge_port)
	message(FATAL_ERROR
		"mcpbridge: `just mcp` opens port ${CMAKE_MATCH_1} and "
		"engine::control::DEFAULT_PORT is ${bridge_port}.\n"
		"  The recipe would start an editor on one port and print a bridge command "
		"for another. Change both, in the same commit.")
endif()

# One Catch2 binary over tests/, the same shape a module gets. `mcpbridge` is a
# bare `add_executable` rather than a `mono_add_library`, so the suite is asked
# for by name here - and it is asked for because the root `AGENTS.md` says every
# public header is covered by a suite and this program had none at all.
#
# It links `Engine::control` for real, unlike the program: the suite drives a
# `Surface` and a `Server` end to end and then puts the byte pump in front of
# them, which is the only way to prove the pump carries a protocol rather than
# merely carrying bytes.
if(MONO_BUILD_TESTS AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
	mono_add_tests(mcpbridge DEPS Engine::control Engine::core Engine::world)

	# The suite spawns the program it is testing, so the program has to be there.
	add_dependencies(test_mcpbridge mcpbridge)
endif()
