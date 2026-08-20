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

# `find_program` caches its answer and does not look again once the variable is
# set - including when the path it cached has since stopped existing. That is an
# ordinary thing to happen rather than an exotic one: a doxygen unpacked into a
# temporary directory, a toolchain on a removable volume, or a package upgraded
# out from under the cache all leave an absolute path that resolves to nothing.
#
# The stale value is still truthy, so the check below is skipped and the failure
# arrives much later as `doxygen -w failed (No such file or directory)` from
# inside RunDoxygen.cmake - a message that names no program, suggests no fix,
# and reads like the docs target is broken rather than the cache being wrong.
# Drop the entry and search again, so the check below is what decides.
foreach(tool MONO_DOXYGEN MONO_DOT)
	if(${tool} AND NOT EXISTS "${${tool}}")
		message(STATUS "docs: cached ${tool} (${${tool}}) is gone - searching again")
		unset(${tool} CACHE)
	endif()
endforeach()

find_program(MONO_DOXYGEN doxygen)
find_program(MONO_DOT dot)

# Doxygen is a documentation prerequisite, not a build one, so a machine without
# it configures and builds and tests exactly as before. What it must not do is
# leave `just docs` reporting "no such target" - that reads as a broken
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
# root, because the root also holds mono.vendor - and a recursive walk of that
# would pull in every markdown file SDL, imgui and shaderc ship.
#
# The order here is the order of the sidebar. Doxygen lists pages as it meets
# them, so the three navigation pages are named individually and first - they
# would otherwise be reached through the recursive `mono.tools` entry at the
# bottom and sort alphabetically, which puts the modules above the guide to
# reading them.
#
# `pages/` holds nothing but `@subpage` lists. Without them every document in
# the repository is a sibling of every other one, and the sidebar is thirty
# entries with no shape. The pages are the only place the grouping exists;
# neither README.md nor any AGENTS.md declares a child, and none of them can be
# asked to - several are checked in as unmodifiable.
#
# They reference pages by the id Doxygen derives from the file's path, because a
# markdown file that cannot be edited cannot be given an explicit `{#id}`. Those
# ids are stable for a Doxygen version and are not promised across one. If an
# upgrade renames them the build says so - an unresolved `@subpage` is a warning
# and `docs-check` fails on warnings - and the current list is:
#
#   just docs && ls .cache/build/dev/docs/html/md_*.html
foreach(root IN ITEMS
	"${CMAKE_SOURCE_DIR}/README.md"
	"${CMAKE_SOURCE_DIR}/mono.tools/docgen/pages/Repository.md"
	"${CMAKE_SOURCE_DIR}/mono.tools/docgen/pages/Standards.md"
	"${CMAKE_SOURCE_DIR}/mono.tools/docgen/pages/Modules.md"
	"${CMAKE_SOURCE_DIR}/AGENTS.md"
	"${CMAKE_SOURCE_DIR}/CONTRIBUTING.md"
	"${CMAKE_SOURCE_DIR}/RUNNING.md"
	"${CMAKE_SOURCE_DIR}/ROADMAP.md"
	# RUNNING.md links to this from its content-origin section, so leaving it out
	# is a dangling link rather than a smaller site - and `docs-check` fails on
	# those. The design notes that used to sit beside it here were deleted in
	# `document-cleanup`; the entry went with them, because an input that does
	# not exist is guarded by `EXISTS` below and therefore silently does nothing,
	# which is how a stale line survives being wrong.
	"${CMAKE_SOURCE_DIR}/SETUP-CDN.md"
	"${CMAKE_SOURCE_DIR}/SECURITY.md"
	"${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
	"${CMAKE_SOURCE_DIR}/docs"
	# The one file, not the tree. AGENTS.md and CONTRIBUTING.md both link to
	# this README, so leaving it out means two dangling links rather than a
	# smaller site - but the skills and commands beside it are instructions to
	# a model, not documentation of the engine.
	"${CMAKE_SOURCE_DIR}/.claude/README.md"
	"${CMAKE_SOURCE_DIR}/mono.engine"
	"${CMAKE_SOURCE_DIR}/mono.client"
	"${CMAKE_SOURCE_DIR}/mono.server"
	# The editor, added at v0.7. A product with public headers is a product
	# whose headers are checked for coverage, which is the point of listing it
	# rather than a cost of doing so.
	"${CMAKE_SOURCE_DIR}/mono.studio"
	"${CMAKE_SOURCE_DIR}/mono.cdn"
	# Presence, added at v0.17. Listed for `mono.studio`'s reason: a member with
	# public headers four programs include is one whose headers are checked for
	# coverage.
	"${CMAKE_SOURCE_DIR}/mono.discord"
	"${CMAKE_SOURCE_DIR}/mono.tools")
	# An input Doxygen cannot read is a warning, and `docs-check` fails on
	# warnings - so a checkout that legitimately does not have one of these yet
	# would fail a check about documentation coverage for a reason that has
	# nothing to do with coverage.
	if(EXISTS "${root}")
		string(APPEND MONO_DOCS_INPUT " \\\n                         \"${root}\"")
	endif()
endforeach()

set(MONO_DOCS_MAINPAGE "${CMAKE_SOURCE_DIR}/README.md")

# An empty markdown file still becomes a page, and a page with no heading in it
# is titled from its filename. Four empty `docs/index.md` placeholders therefore
# put four entries called "index" in the sidebar, each leading nowhere.
#
# The louder half is that every one of them also claimed the page label `index`,
# which the main page already holds. That is where
# `multiple use of section label 'index' for main page` came from, along with the
# index-1.html, index-2.html and index-3.html pages Doxygen invented to tell
# them apart - a warning that reads like a defect in README.md and is nothing of
# the sort.
#
# The condition is emptiness, not the filename. A placeholder that somebody
# fills in gets its page back on the next configure without anyone remembering
# this exists, and a name-based exclusion would go on hiding it silently.
file(GLOB_RECURSE MONO_DOCS_MARKDOWN
	"${CMAKE_SOURCE_DIR}/docs/*.md"
	"${CMAKE_SOURCE_DIR}/mono.engine/*.md"
	"${CMAKE_SOURCE_DIR}/mono.client/*.md"
	"${CMAKE_SOURCE_DIR}/mono.server/*.md"
	"${CMAKE_SOURCE_DIR}/mono.cdn/*.md"
	"${CMAKE_SOURCE_DIR}/mono.tools/*.md")

set(MONO_DOCS_EXCLUDE "")
set(MONO_DOCS_EMPTY "")
foreach(page IN LISTS MONO_DOCS_MARKDOWN)
	file(READ "${page}" contents)
	string(STRIP "${contents}" contents)
	if(contents STREQUAL "")
		string(APPEND MONO_DOCS_EXCLUDE " \\\n                         \"${page}\"")
		file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${page}")
		list(APPEND MONO_DOCS_EMPTY "${relative}")
	endif()
endforeach()

# Said out loud, because a file silently missing from the site is the failure
# this whole block exists to stop happening in the other direction.
if(MONO_DOCS_EMPTY)
	list(JOIN MONO_DOCS_EMPTY ", " joined)
	message(STATUS "docs: skipping empty page(s) - ${joined}")
endif()

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
	"${CMAKE_SOURCE_DIR}/mono.cdn/include"
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
# having done so - see Doxyfile.check.in.
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
