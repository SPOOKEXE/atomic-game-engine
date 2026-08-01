# Generates the HTML header doxygen-awesome-css needs, then runs Doxygen.
#
#   cmake -DDOXYGEN=... -DOUTPUT=... -DHEADER=... -P RunDoxygen.cmake
#   cmake -DDOXYGEN=... -DOUTPUT=... -DCHECK=YES  -P RunDoxygen.cmake
#
# Run by the `docs` and `docs-check` targets. The header is a separate step
# because it has to be produced by the Doxygen that is installed — `doxygen -w`
# stamps its own version into it, and a header from another version warns on
# every run and eventually renders against markup that has moved.
#
# CMake rather than C++, which is the one place mono.tools/AGENTS.md leaves to
# judgement: the rule there is that a tool is C++ when it is a program, and this
# is not a program. It is a few file operations around two process launches, and
# writing it in C++ would mean a second binary that exists to call the first.

cmake_minimum_required(VERSION 3.24)

file(MAKE_DIRECTORY "${OUTPUT}")

# ---------------------------------------------------------------------------
# The coverage pass
# ---------------------------------------------------------------------------
#
# Doxyfile.check is the same configuration with EXTRACT_ALL off, because
# EXTRACT_ALL disables WARN_IF_UNDOCUMENTED and says nothing about having done
# so. See Doxyfile.check.in for why both passes have to exist.
if(CHECK)
	execute_process(
		COMMAND "${DOXYGEN}" "${OUTPUT}/Doxyfile.check"
		WORKING_DIRECTORY "${OUTPUT}"
		RESULT_VARIABLE checked
	)
	if(NOT checked EQUAL 0)
		message(FATAL_ERROR "doxygen failed (${checked}).")
	endif()

	set(gaps "")
	if(EXISTS "${OUTPUT}/gaps.txt")
		file(STRINGS "${OUTPUT}/gaps.txt" gaps)
	endif()
	list(LENGTH gaps count)

	if(count GREATER 0)
		foreach(gap IN LISTS gaps)
			message("${gap}")
		endforeach()
		message(FATAL_ERROR
			"${count} documentation gap(s). Every public entity in a public header "
			"carries a comment; see docs/CODE_DOCUMENTING.md.")
	endif()

	message(STATUS "every public entity is documented")
	return()
endif()

# ---------------------------------------------------------------------------
# The site
# ---------------------------------------------------------------------------

# Doxygen writes over what it generates and removes nothing, so a renamed or
# deleted header leaves its page on the site indefinitely — reachable, stale,
# and worse than a 404 because it still looks current. The generated tree is
# derived, so clearing it is free.
file(REMOVE_RECURSE "${OUTPUT}/html")

# `doxygen -w html <header> <footer> <css>` writes the three default templates.
# Only the header is kept; the footer and stylesheet are the stock ones and
# doxygen-awesome-css replaces the styling anyway.
execute_process(
	COMMAND "${DOXYGEN}" -w html
		"${OUTPUT}/header.default.html"
		"${OUTPUT}/footer.default.html"
		"${OUTPUT}/style.default.css"
	WORKING_DIRECTORY "${OUTPUT}"
	RESULT_VARIABLE written
	OUTPUT_QUIET
)
if(NOT written EQUAL 0)
	message(FATAL_ERROR "doxygen -w failed (${written}).")
endif()

# doxygen-awesome-css is a stylesheet plus four optional behaviours, and the
# behaviours need script tags. This is the only reason a custom header exists.
#
# `$relpath^` is Doxygen's substitution for the path back to the output root,
# so the same header works on a page nested three directories down.
set(SCRIPTS
"<script type=\"text/javascript\" src=\"$relpath^doxygen-awesome-darkmode-toggle.js\"></script>
<script type=\"text/javascript\" src=\"$relpath^doxygen-awesome-fragment-copy-button.js\"></script>
<script type=\"text/javascript\" src=\"$relpath^doxygen-awesome-paragraph-link.js\"></script>
<script type=\"text/javascript\" src=\"$relpath^doxygen-awesome-interactive-toc.js\"></script>
<script type=\"text/javascript\">
    DoxygenAwesomeDarkModeToggle.init()
    DoxygenAwesomeFragmentCopyButton.init()
    DoxygenAwesomeParagraphLink.init()
    DoxygenAwesomeInteractiveToc.init()
</script>
</head>")

file(READ "${OUTPUT}/header.default.html" header)
if(NOT header MATCHES "</head>")
	message(FATAL_ERROR
		"The header Doxygen generated has no </head>. Doxygen ${DOXYGEN} emitted "
		"something this script does not understand.")
endif()
string(REPLACE "</head>" "${SCRIPTS}" header "${header}")
file(WRITE "${HEADER}" "${header}")

execute_process(
	COMMAND "${DOXYGEN}" "${OUTPUT}/Doxyfile"
	WORKING_DIRECTORY "${OUTPUT}"
	RESULT_VARIABLE documented
)
if(NOT documented EQUAL 0)
	message(FATAL_ERROR "doxygen failed (${documented}).")
endif()

# This pass reports malformed documentation — a `@param` naming an argument that
# is not there, a link that does not resolve. It cannot report a *missing*
# comment; that is what `just docs-check` is for.
if(EXISTS "${OUTPUT}/warnings.txt")
	file(STRINGS "${OUTPUT}/warnings.txt" warnings)
	list(LENGTH warnings count)
else()
	set(count 0)
endif()

message(STATUS "docs   ${OUTPUT}/html/index.html")
if(count GREATER 0)
	message(STATUS "errors ${count} — see ${OUTPUT}/warnings.txt")
endif()
