# Functions no program can reach, from the relocations in a build's objects.
#
# A library plus a thin executable, the same shape as `linecount` and
# `sourcecheck`: the ELF reader and the reachability walk are pure functions a
# Catch2 suite can hold, and `main` only finds the files.
#
# The library links nothing. The executable links `Engine::core` for
# `Arguments`, so `deadcode --help` looks like every other `--help` here.
#
# `shared` tier, and unconditional: it reads object files, so every preset can
# build it. Only an unoptimised preset gives a true answer; see `Graph.hpp`.

mono_add_library(deadcode
	TIER shared
	NAMESPACE Tool
)

add_executable(deadcode app/main.cpp)
target_link_libraries(deadcode PRIVATE Tool::deadcode Engine::core)
target_compile_options(deadcode PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(deadcode PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
