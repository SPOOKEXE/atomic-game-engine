# The load-test harness: many real replication clients in one process.
#
# A library plus a thin executable, the same shape as `linecount` and `assetc`.
# What is in the library is the part that is logic rather than input and output -
# the session's phase rule, the statistics, the flags - and that is what its
# `tests/` cover. A harness whose state machine only ran against a live server
# is one whose bugs are indistinguishable from the server's.
#
# **`shared` tier, and the link line is the proof it is headless.** It links no
# `render`, `input`, `audio`, `ui` or `client_lib`: a virtual client is a socket,
# a connector and a store, and anything that needed a device would make two
# hundred of them impossible in one process. `client --headless` still builds a
# GPU device, which is why this is not two hundred of those.
mono_add_library(loadtest
	TIER shared
	NAMESPACE Tool
	DEPS
		Engine::core
		Engine::ecs

		# The wire and what it means. The whole point is that these are the real
		# ones - a stubbed handshake would measure a server nobody runs.
		Engine::net
		Engine::replication

		# The components a snapshot names, the classes an instance names, and the
		# input encoding a client sends. Registering less than
		# `client::BuildReplicatedWorld` does is a harness whose join never
		# completes.
		Engine::scene
		Engine::gui
		Engine::script
		Engine::game
)

add_executable(loadtest app/main.cpp)
target_link_libraries(loadtest PRIVATE Tool::loadtest Engine::core)
target_compile_options(loadtest PRIVATE ${MONO_COMPILE_OPTIONS})

# Tools stage together, apart from the products.
set_target_properties(loadtest PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
