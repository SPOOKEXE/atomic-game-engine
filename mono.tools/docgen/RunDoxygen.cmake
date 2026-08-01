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
# The one warning that is Doxygen's and not ours
# ---------------------------------------------------------------------------
#
# Doxygen names the main page `index`. `MARKDOWN_ID_STYLE = GITHUB` has the
# first heading of the file that becomes the main page claim a GitHub-style id
# in that same scope. They collide, on every run, and the result is:
#
#   README.md:2: warning: multiple use of section label 'index' for main page
#
# Four ways out were measured before this was written, and every one of them
# costs more than the warning does:
#
# - Delete the heading README.md opens with. The warning moves to the next one.
# - `MARKDOWN_ID_STYLE = DOXYGEN`. Silences it by breaking every intra-document
#   link in the repository instead — Doxyfile.in says why those matter.
# - `PROJECT_NAME` spelled exactly as the README's title, which is the advice
#   usually given for this message. No effect: that advice is about the explicit
#   `@mainpage` tag, and the main page here comes from a markdown file.
# - `USE_MDFILE_AS_MAINPAGE` off. This is the one that does work — nothing warns
#   — and it works by throwing away the front page, so the site opens on a file
#   list instead of the README. Not a trade worth making for one line of log.
#
# So the line is dropped, because a check that cannot ever pass stops being read
# and takes the real warnings down with it. It is dropped *loudly*: the count is
# reported, so the day a newer Doxygen stops emitting it, the number goes to
# zero, somebody notices, and this whole block can go.
#
# Matched on the text rather than the filename, so that it stays specific to
# this defect and does not quietly start excusing anything else README.md does.
function(_mono_filter_known_artefacts path out_suppressed)
	set(${out_suppressed} 0 PARENT_SCOPE)
	if(NOT EXISTS "${path}")
		return()
	endif()

	file(READ "${path}" raw)

	# Counted before the removal, and on the whole text rather than a CMake list
	# — a warning quoting C++ can contain a semicolon, and `foreach(IN LISTS)`
	# would split one line into two.
	string(REGEX MATCHALL "multiple use of section label 'index' for main page" hits "${raw}")
	list(LENGTH hits found)
	if(found EQUAL 0)
		return()
	endif()

	string(REGEX REPLACE
		"[^\n]*multiple use of section label 'index' for main page[^\n]*\n?"
		"" raw "${raw}")
	file(WRITE "${path}" "${raw}")
	set(${out_suppressed} ${found} PARENT_SCOPE)
endfunction()

# Lines remaining in a log, counted without building a list for the same reason.
function(_mono_count_lines path out_count)
	set(${out_count} 0 PARENT_SCOPE)
	if(NOT EXISTS "${path}")
		return()
	endif()
	file(READ "${path}" raw)
	string(REGEX MATCHALL "[^\n]+" lines "${raw}")
	list(LENGTH lines n)
	set(${out_count} ${n} PARENT_SCOPE)
endfunction()

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

	# The coverage pass reads the same markdown and hits the same main-page
	# collision, so it needs the same line removed — otherwise `docs-check`
	# reports it as a documentation gap, which it is not.
	_mono_filter_known_artefacts("${OUTPUT}/gaps.txt" ignored)

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
_mono_filter_known_artefacts("${OUTPUT}/warnings.txt" suppressed)
_mono_count_lines("${OUTPUT}/warnings.txt" count)

message(STATUS "docs   ${OUTPUT}/html/index.html")
if(count GREATER 0)
	message(STATUS "errors ${count} — see ${OUTPUT}/warnings.txt")
endif()
if(suppressed GREATER 0)
	message(STATUS
		"note   ${suppressed} known Doxygen main-page label warning(s) dropped "
		"— RunDoxygen.cmake says why, and what makes it removable")
endif()
