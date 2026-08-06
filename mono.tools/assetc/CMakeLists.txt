# The asset compiler — a directory of source art in, a directory a publisher can
# serve out.
#
# A library plus a thin executable, the same shape as linecount and testrunner
# and for the same reason: the interesting half is a set of judgements — what a
# baked asset is called, how a model's texture references are rewritten, what
# happens to a file the baker does not understand — and a judgement inside a
# `main.cpp` is one nothing can test.
#
# This is the program `engine::bake`'s missing filesystem is missing *for*. The
# module has no `std::filesystem` anywhere in it so that its whole pipeline is
# testable without opening a file; everything that opens one is here.
#
# `shared` tier: no client or server code, and it is not shipped with a game.
mono_add_library(assetc
	TIER shared
	NAMESPACE Tool
	DEPS
		Engine::core
		Engine::assets
		Engine::bake
)

add_executable(assetc app/main.cpp)
target_link_libraries(assetc PRIVATE Tool::assetc Engine::core Engine::assets Engine::bake)
target_compile_options(assetc PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(assetc PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
