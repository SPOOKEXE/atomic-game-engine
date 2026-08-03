# The line counter — empty, comment and code, as markdown.
#
# A library plus a thin executable, the same shape as testrunner and docgen and
# for the same reason: the interesting half is a pure function from a string to
# three numbers, and a function inside a main.cpp is one nothing can test. The
# classification is a judgement — a blank line inside a block comment, a `//`
# inside a raw string of GLSL — and a judgement with no suite behind it drifts.
#
# `shared` tier: it links no client or server code, and it is not shipped.

mono_add_library(linecount
	TIER shared
	NAMESPACE Tool
	# Arguments only, so that `linecount --help` looks like every other --help
	# in the repository.
	DEPS Engine::core
)

add_executable(linecount app/main.cpp)
target_link_libraries(linecount PRIVATE Tool::linecount Engine::core)
target_compile_options(linecount PRIVATE ${MONO_COMPILE_OPTIONS})

# Tools stage together, apart from the products. A tool is not a program a
# player runs, so it does not get a staged tree of its own.
set_target_properties(linecount PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
