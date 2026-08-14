# The selective benchmark runner.
#
# A thin executable over `Tool::testrunner` and nothing else. Discovery, the
# cascading signature and the skip cache are that library's and are used here
# unchanged - a second implementation of "what could this change have affected"
# would be a second thing to keep correct, and the neglected one would be the
# one that silently stopped re-running.
#
# What differs is only what a run produces: a benchmark answers "how long", not
# "did it hold", so there is a baseline instead of a pass.

add_executable(benchrunner app/main.cpp)
target_link_libraries(benchrunner PRIVATE Tool::testrunner Engine::core)
target_compile_options(benchrunner PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(benchrunner PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
