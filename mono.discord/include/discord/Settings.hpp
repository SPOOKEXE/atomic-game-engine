#pragma once

// What somebody configured, and the flag table every program reads it from.
//
// One table, declared by whichever program wants presence, so the studio's tab,
// a config file and a command line are three ways to fill one struct rather
// than three things to keep in step.
//
// ## The Application ID is not a secret
//
// It looks like one sitting in a preferences file, which is why this is written
// down rather than assumed. A Discord Application ID ships inside every game
// that uses rich presence and is visible to anybody watching the socket; it
// names an application the way a package name does. `studio/Config.hpp` is
// explicit that a *signing seed* must never live in preferences, and that rule
// is about a thing which authorises. This one does not.
//
// ## Off by default, and inert without an id
//
// Rich presence publishes the name of the file somebody has open to their whole
// friends list. That is opt-in or it is a surprise, and `HideNames` is here for
// the case where somebody wants the feature and not the file name.
//
// @tier shared
// @since v0.17

#include <string>

namespace discord {

	// The application id a build ships with, and it is not one.
	//
	// **A placeholder rather than an empty string, so an unconfigured install
	// says so out loud.** Discord identifies an application by a snowflake -
	// seventeen to nineteen decimal digits - and `-1` is not one and never will
	// be, which is what makes it safe to treat as "nobody has set this" without
	// a second flag to say so. `IsConfigured` refuses it exactly as it refuses
	// an empty field, so a person who ticks the switch without pasting an id
	// gets "no application id" rather than a handshake Discord answers with
	// `Invalid Client ID`.
	//
	// There is deliberately no real default. An id names *whose* application a
	// card reports as, and the bold first line every friend sees is that
	// application's name - so shipping one would put this project's name on
	// somebody else's game.
	//
	// @since v0.17
	inline constexpr const char *UNSET_APPLICATION_ID = "-1";

	// What presence reports, and whether it reports at all.
	//
	// @since v0.17
	struct Settings {
		// Whether to connect. Off unless somebody asked for it.
		bool Enabled = false;

		// The Discord Application ID, as decimal digits.
		//
		// Empty or `UNSET_APPLICATION_ID` is inert: the link never opens a
		// socket and the tab says why. Both, rather than one, because a field
		// somebody cleared and a field nobody has touched mean the same thing
		// here and should behave the same way.
		std::string ApplicationId = UNSET_APPLICATION_ID;

		// The template for the first line. See `Fill`.
		std::string Details;

		// The template for the second line.
		std::string State;

		// An asset key uploaded to the Discord application, not a URL.
		std::string LargeImage = "atomic";

		// The tooltip on that image.
		std::string LargeText = "Atomic Game Engine";

		// The button, or an empty label for none.
		//@{
		std::string ButtonLabel;
		std::string ButtonUrl;
		//@}

		// Whether Discord draws the "01:23 elapsed" timer.
		bool ShowElapsed = true;

		// Whether place, world, game and store names are replaced with a
		// generic word before any template sees them.
		bool HideNames = false;

		// Whether to advertise a join secret and subscribe to the join event.
		//
		// **Off, and it stays off until something can act on a secret.**
		// Discord delivers a join by launching the game through a registered
		// URI scheme, and no Atomic program registers one yet. The path is
		// built and tested; what is missing is the handler, which is its own
		// piece of work on three platforms.
		bool JoinSecrets = false;

		// Whether two settings would produce the same behaviour.
		//
		// @return `true` when every field matches.
		bool operator==(const Settings &) const = default;
	};

	// Declares this module's flag table. Every row in it is named with a
	// discord dot prefix, so a config file groups them together.
	//
	// Called by a program that wants presence, beside its own `DeclareFlags`.
	//
	// @return `false` when a name collided, which is a bug in a table.
	// @since v0.17
	bool DeclareFlags();

	// `defaults` with whatever the flags say written over it.
	//
	// **Returns `defaults` untouched when nothing declared the table**, which
	// is `engine::parallel::ApplyFlags`' rule: a program that never registered
	// these does not use them, and a dead flag reads `false` - which would be
	// the right answer for the wrong reason.
	//
	// @param defaults This program's built-in templates.
	// @return The settings to run with.
	// @since v0.17
	Settings SettingsFromFlags(const Settings &defaults);

	// Whether these settings could ever produce a connection.
	//
	// @param settings What was configured.
	// @return `true` when enabled and carrying an application id that is
	//         neither empty nor `UNSET_APPLICATION_ID`.
	// @since v0.17
	bool IsConfigured(const Settings &settings);
}
