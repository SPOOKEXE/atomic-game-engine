# The documentation filter, and the `docs` target that drives Doxygen with it.
#
# A library plus a thin executable, the same shape as testrunner and for the
# same reason: the interesting half is a pure string-to-string function, and a
# function inside a main.cpp is one nothing can test.
#
# `shared` tier: it links no client or server code, and it is not shipped.

mono_add_library(docgen
	TIER shared
	NAMESPACE Tool
	# Arguments only, so that `docgen --help` looks like every other --help in
	# the repository even though Doxygen is the only caller that matters.
	DEPS Engine::core
)

add_executable(docgen app/main.cpp)
target_link_libraries(docgen PRIVATE Tool::docgen Engine::core)
target_compile_options(docgen PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(docgen PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")

# ---------------------------------------------------------------------------
# The docs target
# ---------------------------------------------------------------------------

find_program(MONO_DOXYGEN doxygen)
find_program(MONO_DOT dot)

# Doxygen is a documentation prerequisite, not a build one, so a machine without
# it configures and builds and tests exactly as before. What it must not do is
# leave `just docs` reporting "no such target" — that reads as a broken
# repository rather than a missing program.
if(NOT MONO_DOXYGEN)
	# One command, so that the line the build system echoes back on failure is
	# short enough that the sentence above it is still the thing you read.
	add_custom_target(docs
		COMMAND "${CMAKE_COMMAND}" -E false
		COMMENT "docs: no doxygen on PATH. `apt install doxygen graphviz`, or `brew install doxygen graphviz`. Only this target needs it."
		VERBATIM
		USES_TERMINAL
	)
	return()
endif()

set(MONO_DOCS_OUTPUT "${CMAKE_BINARY_DIR}/docs")
set(MONO_DOCS_HEADER "${MONO_DOCS_OUTPUT}/header.html")
set(MONO_DOCGEN_BINARY "${MONO_STAGE_ROOT}/tools/docgen")

# One entry per line in the generated Doxyfile, quoted, because a checkout path
# with a space in it is otherwise several inputs.
set(MONO_DOCS_INPUT "")
#
# The repository's prose is listed file by file rather than by pointing at the
# root, because the root also holds mono.vendor — and a recursive walk of that
# would pull in every markdown file SDL, imgui and shaderc ship.
foreach(root IN ITEMS
	"${CMAKE_SOURCE_DIR}/README.md"
	"${CMAKE_SOURCE_DIR}/AGENTS.md"
	"${CMAKE_SOURCE_DIR}/CONTRIBUTING.md"
	"${CMAKE_SOURCE_DIR}/RUNNING.md"
	"${CMAKE_SOURCE_DIR}/ROADMAP.md"
	"${CMAKE_SOURCE_DIR}/SECURITY.md"
	"${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
	"${CMAKE_SOURCE_DIR}/docs"
	# The one file, not the tree. AGENTS.md and CONTRIBUTING.md both link to
	# this README, so leaving it out means two dangling links rather than a
	# smaller site — but the skills and commands beside it are instructions to
	# a model, not documentation of the engine.
	"${CMAKE_SOURCE_DIR}/.claude/README.md"
	"${CMAKE_SOURCE_DIR}/mono.engine"
	"${CMAKE_SOURCE_DIR}/mono.client"
	"${CMAKE_SOURCE_DIR}/mono.server"
	"${CMAKE_SOURCE_DIR}/mono.tools")
	# An input Doxygen cannot read is a warning, and `docs-check` fails on
	# warnings — so a checkout that legitimately does not have one of these yet
	# would fail a check about documentation coverage for a reason that has
	# nothing to do with coverage.
	if(EXISTS "${root}")
		string(APPEND MONO_DOCS_INPUT " \\\n                         \"${root}\"")
	endif()
endforeach()

set(MONO_DOCS_MAINPAGE "${CMAKE_SOURCE_DIR}/README.md")

# Same reason: PROJECT_LOGO naming a file that is not there is a warning.
set(MONO_DOCS_LOGO "")
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.png")
	set(MONO_DOCS_LOGO "${CMAKE_SOURCE_DIR}/assets/icon.png")
endif()

# Every public include root, so that a page shows the include line a caller
# would write. Globbed rather than listed: a module added without a line here
# would silently get the wrong include shown, and nothing would fail.
file(GLOB MONO_INCLUDE_ROOTS
	"${CMAKE_SOURCE_DIR}/mono.engine/*/include"
	"${CMAKE_SOURCE_DIR}/mono.client/include"
	"${CMAKE_SOURCE_DIR}/mono.server/include"
	"${CMAKE_SOURCE_DIR}/mono.tools/*/include"
	"${CMAKE_SOURCE_DIR}/mono.build/*/include")
set(MONO_DOCS_INCLUDE_ROOTS "")
foreach(root IN LISTS MONO_INCLUDE_ROOTS)
	string(APPEND MONO_DOCS_INCLUDE_ROOTS " \\\n                         \"${root}\"")
endforeach()

set(MONO_AWESOME "${CMAKE_SOURCE_DIR}/mono.vendor/doxygen-awesome-css")
set(MONO_DOCS_STYLESHEETS
	"\"${MONO_AWESOME}/doxygen-awesome.css\" \\\n                         \"${MONO_AWESOME}/doxygen-awesome-sidebar-only.css\" \\\n                         \"${MONO_AWESOME}/doxygen-awesome-sidebar-only-darkmode-toggle.css\"")

# Copied beside the HTML so the generated header can load them by name.
set(MONO_DOCS_SCRIPT_FILES
	"${MONO_AWESOME}/doxygen-awesome-darkmode-toggle.js"
	"${MONO_AWESOME}/doxygen-awesome-fragment-copy-button.js"
	"${MONO_AWESOME}/doxygen-awesome-paragraph-link.js"
	"${MONO_AWESOME}/doxygen-awesome-interactive-toc.js")
set(MONO_DOCS_SCRIPTS "")
foreach(script IN LISTS MONO_DOCS_SCRIPT_FILES)
	string(APPEND MONO_DOCS_SCRIPTS " \\\n                         \"${script}\"")
endforeach()

if(MONO_DOT)
	set(MONO_DOCS_HAVE_DOT YES)
else()
	set(MONO_DOCS_HAVE_DOT NO)
	message(STATUS "  docs       graphviz not found; the site will have no diagrams")
endif()

if(NOT EXISTS "${MONO_AWESOME}/doxygen-awesome.css")
	message(FATAL_ERROR
		"mono.vendor/doxygen-awesome-css is empty. Run `just setup`.")
endif()

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
	"${MONO_DOCS_OUTPUT}/Doxyfile"
	@ONLY
)

# The same configuration with EXTRACT_ALL off. It has to be a second pass
# because EXTRACT_ALL disables WARN_IF_UNDOCUMENTED and reports nothing about
# having done so — see Doxyfile.check.in.
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.check.in"
	"${MONO_DOCS_OUTPUT}/Doxyfile.check"
	@ONLY
)

# The header is generated rather than checked in. `doxygen -w` emits one that
# matches the installed version; a checked-in header written against a different
# one warns on every run and eventually renders wrong. This way a contributor on
# 1.9 and one on 1.14 both get a correct page.
add_custom_target(docs
	COMMAND "${CMAKE_COMMAND}"
		-DDOXYGEN=${MONO_DOXYGEN}
		-DOUTPUT=${MONO_DOCS_OUTPUT}
		-DHEADER=${MONO_DOCS_HEADER}
		-P "${CMAKE_CURRENT_SOURCE_DIR}/RunDoxygen.cmake"
	DEPENDS docgen
	COMMENT "Documenting ${PROJECT_NAME} ${PROJECT_VERSION}"
	VERBATIM
	USES_TERMINAL
)

# Generates no HTML, which is most of the reason a second parse is nearly free.
add_custom_target(docs-check
	COMMAND "${CMAKE_COMMAND}"
		-DDOXYGEN=${MONO_DOXYGEN}
		-DOUTPUT=${MONO_DOCS_OUTPUT}
		-DCHECK=YES
		-P "${CMAKE_CURRENT_SOURCE_DIR}/RunDoxygen.cmake"
	DEPENDS docgen
	COMMENT "Checking documentation coverage"
	VERBATIM
	USES_TERMINAL
)
