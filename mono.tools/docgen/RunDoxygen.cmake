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

# Lines in a log, counted without building a CMake list — a warning quoting C++
# can contain a semicolon, and `foreach(IN LISTS)` would split one line into two.
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
    // **Dark on a first visit, whatever the reader's operating system says.**
    // doxygen-awesome follows `prefers-color-scheme`, so a machine configured
    // light served a light site — the one surface of this project that did not
    // match the rest of it, since `ui::Palette` has no light theme at all.
    //
    // Seeded once and never again, which is the whole reason for the extra key.
    // The toggle records a *disagreement* with the system rather than a choice:
    // on a light system, \"I want dark\" is a stored key and \"I want light\" is
    // its absence — indistinguishable from never having visited. Without a
    // marker of our own, writing the preference on every load would overwrite
    // the reader's choice each time they picked light and the toggle would
    // appear not to work.
    try {
        if (!localStorage.getItem(\"mono-theme-seeded\")) {
            localStorage.setItem(\"mono-theme-seeded\", \"1\")
            localStorage.setItem(DoxygenAwesomeDarkModeToggle.prefersDarkModeInLightModeKey, true)
            localStorage.removeItem(DoxygenAwesomeDarkModeToggle.prefersLightModeInDarkModeKey)
        }
    } catch (e) {
        // Private browsing refuses `localStorage`. The site still works and
        // still toggles; it just opens in whatever the system prefers.
    }

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
_mono_count_lines("${OUTPUT}/warnings.txt" count)

message(STATUS "docs   ${OUTPUT}/html/index.html")
if(count GREATER 0)
	message(STATUS "errors ${count} — see ${OUTPUT}/warnings.txt")
endif()
