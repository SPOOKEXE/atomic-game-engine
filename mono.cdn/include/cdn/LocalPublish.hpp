#pragma once

// Applying the origin's publishing policy to the engine-owned local content
// workspace. The workspace layout is shared by every program and therefore
// lives in `Engine::assets`; grouping, compression and publication remain CDN
// policy and stay here.
//
// @tier shared

#include <engine/assets/LocalStore.hpp>

#include <cdn/Publisher.hpp>
#include <cstdint>
#include <optional>

namespace cdn {

	// Publishes everything in `baked/` into `processed/`.
	//
	// An empty `baked/` beside a full `raw/` is refused rather than replacing a
	// working manifest with an empty one. The signing key remains the caller's
	// because a key beside the content it signs would trust anything written
	// there.
	//
	// @param paths The local content workspace.
	// @param signing The key to sign the manifest with.
	// @param seconds The time to record in the workspace log.
	// @param settings How the CDN chunks, groups and compresses the content.
	// @return The publish report, or nothing when publication failed.
	// @since v0.10
	std::optional<PublishReport> PublishLocal(
		const engine::assets::LocalPaths &paths,
		const engine::assets::SigningKey &signing,
		uint64_t seconds,
		const PublishSettings &settings = {}
	);
}
