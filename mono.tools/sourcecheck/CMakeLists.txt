# The four architecture rules that live in source text, not in the target graph.
#
# A library plus a thin executable, the same shape as `linecount` and
# `shadercheck` and for the same reason: the interesting half is a pure function
# from a tree of text to a list of findings, and a function inside a `main.cpp`
# is one nothing can test. Its fixtures are source trees under `tests/fixtures/`,
# so every negative case exists as an input rather than as a sentence in a
# comment.
#
# **C++ rather than a script, which `mono.tools/AGENTS.md` decides rather than
# this file.** A tool is C++ when it is a program and CMake when its input is
# CMake's own output. This one reads first-party sources, which is neither of
# those inputs, so it is a program - and it gets a Catch2 suite the way every
# other program here does.
#
# **The library links nothing at all, and that is deliberate.** It reads C++ as
# text, so a dependency on the engine would be a dependency on somebody else's
# opinion about strings. The executable links `Engine::core` for `Arguments`, so
# `sourcecheck --help` looks like every other `--help` in the repository.
#
# `shared` tier, and unconditional: it needs no device, no shader compiler and
# nothing built, so a `server` or `cdn` preset checks the same rules the client
# preset does over the same tree.

mono_add_library(sourcecheck
	TIER shared
	NAMESPACE Tool
)

add_executable(sourcecheck app/main.cpp)
target_link_libraries(sourcecheck PRIVATE Tool::sourcecheck Engine::core)
target_compile_options(sourcecheck PRIVATE ${MONO_COMPILE_OPTIONS})

# Tools stage together, apart from the products. A tool is not a program a
# player runs, so it does not get a staged tree of its own.
set_target_properties(sourcecheck PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
