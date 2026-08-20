#pragma once

// What Discord says this origin is serving.
//
// **A small owner rather than a `Link` in `main`**, because the wording, the
// tokens and the epoch second are three things that belong together and none of
// them belongs in a serve loop.
//
// This program has no preferences page, so everything it reports comes from the
// `discord` flag table. `mono.studio` is the one program that configures it
// in an interface, and the reasoning is in `studio/Config.hpp`.
//
// @tier shared
// @since v0.17

#include <cdn/Service.hpp>
#include <cstdint>
#include <discord/Link.hpp>
#include <memory>

namespace cdn {

	// The presence an origin reports, when one was asked for.
	//
	// @since v0.17
	class DiscordPresence {
	  public:
		// Makes one, if the flags asked for it.
		//
		// **Null is the ordinary answer**, and every caller treats it as
		// "nothing to do" rather than as a failure: an origin nobody configured
		// this for allocates nothing and opens no socket.
		//
		// @return The reporter, or `nullptr`.
		static std::unique_ptr<DiscordPresence> Start();

		// Says what is being served, and keeps the connection alive.
		//
		// Safe every pass of a serve loop: `discord::Link` sends only what
		// changed and only as often as the protocol allows.
		//
		// @param port       The port this origin answers on.
		// @param counters   What it has served.
		// @param nowSeconds This process's monotonic clock.
		void Pump(uint16_t port, const ServiceCounters &counters, double nowSeconds);

	  private:
		explicit DiscordPresence(discord::Settings settings, int64_t startedUnixSeconds);

		discord::Link Wire;
		int64_t Started = 0;
	};
}
