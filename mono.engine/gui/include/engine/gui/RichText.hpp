#pragma once

// arch-waiver public-header: forward GUI API. Interface compilers need this
// complete rich-text value contract at their public boundary.

// Markup in a `TextLabel`, turned into plain text and the styles over it.
//
// **The parse produces spans and never geometry.** The canonical shaper lays
// out the plain string with these byte ranges, then gives every painter the
// same glyph positions.
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
// Malformed markup within the text limit is shown literally, tags and all.
// Inputs beyond the shaping limit are refused with empty output. Nesting and
// styled span counts are bounded at 64; exceeding either shows the bounded
// source literally.
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
	// @return `false` when malformed or over a limit. A bounded malformed
	//         source is copied literally into `plain`. Oversized source leaves
	//         `plain` empty. `spans` is empty in either case.
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
