# Compiled SPIR-V against the resource contract SDL_CreateGPUShader documents.
#
# A library plus a thin executable, the same shape as linecount and for the same
# reason: the interesting half is a pure function from a module's bytes to a list
# of findings, and a function inside a main.cpp is one nothing can test. Its
# suites build SPIR-V word by word, so the negative cases - a missing binding, a
# gap in a set, a fragment shader in a vertex shader's sets - exist as inputs
# rather than as sentences in a comment.
#
# **The library links nothing at all, and that is deliberate.** It reads a byte
# stream that a specification defines, so a dependency on the engine would only
# be a dependency on somebody else's opinion about strings. The executable links
# `Engine::core` for `Arguments`, so `shadercheck --help` looks like every other
# `--help` in the repository.
#
# `shared` tier, and unconditional: it needs no shader compiler, which is the
# point of reading the binary rather than running one. A `server` preset builds
# it and stages no shaders for it to read, and `just shader-check` says so.

mono_add_library(shadercheck
	TIER shared
	NAMESPACE Tool
)

add_executable(shadercheck app/main.cpp)
target_link_libraries(shadercheck PRIVATE Tool::shadercheck Engine::core)
target_compile_options(shadercheck PRIVATE ${MONO_COMPILE_OPTIONS})

# Tools stage together, apart from the products. A tool is not a program a
# player runs, so it does not get a staged tree of its own.
set_target_properties(shadercheck PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
