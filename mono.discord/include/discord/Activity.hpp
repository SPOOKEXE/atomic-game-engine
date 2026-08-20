#pragma once

// What Discord is asked to show, and the rules for filling it in.
//
// An `Activity` is already resolved: templates have been substituted and every
// string is already inside Discord's limit. That is deliberate. The alternative
// - handing the socket a template and a bag of facts - would put substitution
// on the wire path, where a mistake is a frame Discord silently drops rather
// than a preview that looks wrong.
//
// ## Level-triggered, not a stream
//
// An activity is a statement of what is true *now*. It is not an event and
// there is no history. Everything downstream leans on that: `Link` compares the
// activity against the last one it sent and stays quiet when they match, and a
// frame that could not be written is dropped rather than queued, because the
// next pump re-states the same truth.
//
// @tier shared
// @since v0.17

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace discord {

	// What Discord truncates, so what this truncates first.
	//
	// **Ours rather than theirs, deliberately.** A field over the limit is not
	// an error Discord reports - the activity is rejected whole and nothing
	// says why - so the only way to find out is to not do it.
	//@{
	inline constexpr size_t TEXT_LIMIT = 128;
	inline constexpr size_t IMAGE_KEY_LIMIT = 256;
	inline constexpr size_t BUTTON_LABEL_LIMIT = 32;
	inline constexpr size_t BUTTON_URL_LIMIT = 512;
	inline constexpr size_t MAXIMUM_BUTTONS = 2;
	//@}

	// One button on a presence card.
	//
	// **Discord shows these to everybody except the person whose presence it
	// is.** That is not a bug to work around and it is the single most
	// confusing thing about this feature: somebody who sets a button, looks at
	// their own profile and sees nothing concludes the whole thing is broken.
	// The studio's tab says so beside the field.
	//
	// @since v0.17
	struct Button {
		// What the button says. At most `BUTTON_LABEL_LIMIT` characters.
		std::string Label;

		// Where it goes. `https://` only; Discord refuses anything else.
		std::string Url;

		bool operator==(const Button &) const = default;
	};

	// What Discord is asked to show.
	//
	// @since v0.17
	struct Activity {
		// The first line under the application's name.
		std::string Details;

		// The second line.
		std::string State;

		// An asset key uploaded to the Discord application, not a URL.
		std::string LargeImage;

		// The tooltip on that image.
		std::string LargeText;

		// When this started, as a unix epoch second. Zero draws no timer.
		//
		// **Supplied rather than read**, which is what keeps this module free
		// of a clock. `Link::Pump` takes monotonic seconds and Discord wants
		// wall-clock ones, and the caller is the only thing that has both.
		int64_t StartedUnixSeconds = 0;

		// The party this process is part of, for the join card.
		//
		// A zero `PartySize` writes no party block at all. Discord draws
		// "2 of 8" from these, and a party with no capacity is not a thing it
		// can render.
		//@{
		std::string PartyId;
		uint32_t PartySize = 0;
		uint32_t PartyCapacity = 0;
		//@}

		// What a joiner is handed when they accept. Empty unless
		// `Settings::JoinSecrets` is on.
		std::string JoinSecret;

		// At most `MAXIMUM_BUTTONS`. Anything past that is dropped.
		std::vector<Button> Buttons;

		// Whether there is anything at all to say.
		//
		// @return `true` when every field a person would see is empty.
		bool IsEmpty() const;

		bool operator==(const Activity &) const = default;
	};

	// Shortens `text` to at most `limit` characters without splitting one.
	//
	// **Counted in UTF-8 code points rather than bytes, and cut on a boundary.**
	// Discord's limits are in characters, and a cut through the middle of a
	// multibyte sequence produces a payload its JSON parser rejects - which
	// looks exactly like "rich presence does not work" and says nothing about
	// the emoji in somebody's place name.
	//
	// @param text  What to shorten.
	// @param limit The most characters to keep.
	// @return `text` when it already fits, and a prefix of it otherwise.
	// @since v0.17
	std::string Clamp(std::string_view text, size_t limit);

	// What a program has to say about itself, as tokens a template can name.
	//
	// A flat list rather than a map: there are four or five of these, they are
	// rebuilt every pump, and a linear scan over five pairs beats hashing five
	// strings to look up five strings.
	//
	// @since v0.17
	using Facts = std::vector<std::pair<std::string, std::string>>;

	// Substitutes `{token}` throughout `pattern` and clamps the result.
	//
	// **An unknown token resolves to nothing rather than to itself.** A
	// template naming a token this program does not publish is somebody who
	// copied the server's line into the studio's box, and showing them
	// `{capacity}` on their own profile is worse than showing them a gap.
	//
	// A `{` with no closing `}` is literal, so a template is never made
	// unwritable by a typo.
	//
	// @param pattern The template.
	// @param facts   What the tokens resolve to.
	// @param limit   The most characters to keep, per `Clamp`.
	// @return The filled text.
	// @since v0.17
	std::string Fill(std::string_view pattern, const Facts &facts, size_t limit);
}
