#pragma once

// arch-waiver public-header: forward GUI API. Interface compilers need this
// complete rich-text value contract at their public boundary.

// Markup in a `TextLabel`, turned into plain text and the styles over it.
//
// **The parse produces spans and never geometry.** A rich-text run reaches a
// backend as one string plus a list of byte ranges, and the backend lays it out
// in one pass with its own glyph metrics - which is the only arrangement under
// which the second word lands where the renderer thinks it does. Positioning
// the runs here would use `AVERAGE_ADVANCE`, and `Layout.hpp`'s standing rule is
// that there is exactly one answer to how wide a string is; two would show up as
// emphasis drifting out of place as a panel resizes. `DrawSpan` says the same
// thing from the other end.
//
// ## What is understood, and what a malformed string does
//
// Roblox's tag set, less the ones that need a font catalogue this engine does
// not have:
//
//   - `<b>`, `<i>`, `<u>`, `<s>` and their closers
//   - `<font color="#RRGGBB" size="16" transparency="0.5" face="Bold">`
//   - `<br />`
//   - the entities `&lt; &gt; &amp; &quot; &apos;` and `&#NNN;`
//
// **A string that does not parse is shown literally, tags and all**, which is
// Roblox's behaviour and the useful one: an author who typed `a < b` sees
// `a < b` rather than nothing, and an author who mistyped a tag sees the tag and
// knows where to look. The alternative - dropping what did not parse - hides a
// mistake at exactly the moment somebody is making it.
//
// @tier L7 · shared

#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace engine::gui {

	// Strips markup from `source` and lists the styles it asked for.
	//
	// @param source The authored string, markup and all.
	// @param base   The label the run belongs to. Read for the colour, face,
	//        size and transparency a span inherits when it overrides only some
	//        of them.
	// @param plain  Filled with the text a reader sees. Cleared first.
	// @param spans  Filled with the styled ranges over `plain`, in order and
	//        without overlaps. Cleared first. Empty when the string asked for no
	//        styling at all, which is the ordinary case and costs a backend
	//        nothing.
	// @return `false` when the markup is malformed. `plain` is then `source`
	//         unchanged and `spans` is empty, so a caller that ignores the
	//         result still shows something a person can read.
	// @since v0.18
	bool ParseRichText(
		std::string_view source, const Label &base, std::string &plain, std::vector<DrawSpan> &spans
	);

	// The first `count` characters of a UTF-8 string.
	//
	// **Characters and not bytes**, which is `Label::MaxVisible`'s whole point:
	// a typewriter effect that counted bytes would reveal half of an accented
	// letter. Negative counts mean the whole string, which is the property's
	// own "no limit" value rather than a special case a caller has to remember.
	//
	// @param text  The string.
	// @param count How many characters to keep, or a negative number for all.
	// @return A view of `text` from its start. Never splits a sequence.
	// @since v0.18
	std::string_view FirstCharacters(std::string_view text, int32_t count);
}
