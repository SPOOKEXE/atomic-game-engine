#pragma once

// The origin's own settings, and the one place its defaults are written down.
//
// `client/Settings.hpp` carries the whole argument. Two differences worth
// naming here, because both are about what an origin is:
//
// - **This program has two settings structs and they stay two.** `CDNSettings`
//   is the request pipeline and `ServiceSettings` is the socket in front of it,
//   and `cdn/Service.hpp` keeps them apart deliberately. So there are two
//   readers here rather than one, and neither invents a combined type.
// - **The two secrets are settings and the signing key is not.** `--grant-key`
//   and `--ingest-key` belong to a deployment and are exactly what a config
//   file is for; a *signing* key belongs to whoever publishes the game, and
//   `assets/AGENTS.md` records that an origin holds none. Putting it in a file
//   an origin reads would be putting it on every serving box, permanently,
//   which is the thing publishing is a separate mode to avoid.
//
// @since v0.15

#include <cdn/Origin.hpp>
#include <cdn/Service.hpp>

namespace cdn {

	// Declares the origin's own settings.
	//
	// **Not the publish gates**, which are `assets::DeclareContentFlags`'s and
	// are declared beside this by whoever publishes.
	//
	// @return `false` when a name collided, which is a bug in a table.
	bool DeclareFlags();

	// The request pipeline's settings, filled from the flags.
	CDNSettings OriginFromFlags();

	// The socket's settings, filled from the flags.
	//
	// **The inbox is left empty**, because where an upload lands is derived
	// from the store path and only `main` knows what that turned out to be.
	ServiceSettings ServiceFromFlags();
}
