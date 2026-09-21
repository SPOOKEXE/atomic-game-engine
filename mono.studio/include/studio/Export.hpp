#pragma once

// arch-waiver public-header: forward studio API. Publishing integrations use
// this complete export contract.

// Frozen Studio export requests and their read-only preflight.

#include <engine/delivery/Client.hpp>
#include <engine/game/Project.hpp>
#include <engine/world/Universe.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <studio/ContentSources.hpp>
#include <vector>

namespace studio {
	// Named phases shown by the export progress UI.
	//
	// @since v0.21
	enum class ExportPhase : uint8_t {
		Idle,
		SerializeWorlds,
		ValidateCatalogue,
		FetchAssets,
		CopyRawFiles,
		ValidateCdnSources,
		BuildArchive,
		VerifyArchive,
		PublishResult,
		Complete,
		Failed,
		Cancelled,
	};

	// Returns the stable progress label shown for an export phase.
	const char *Describe(ExportPhase phase);

	// Mutable choices collected by the export dialog.
	//
	// @since v0.21
	struct ExportOptions {
		// Artifact format selected by the export dialog.
		engine::game::ExportProduct Product = engine::game::ExportProduct::WorldFile;
		// Whether include processed assets.
		bool IncludeProcessedAssets = false;
		// Whether include raw authoring.
		bool IncludeRawAuthoring = false;
		// Whether include public cdns.
		bool IncludePublicCdns = false;
		// Whether validate cdn configuration.
		bool ValidateCdnConfiguration = true;
		// Whether check remote reachability.
		bool CheckRemoteReachability = false;
		// Whether require complete catalogue.
		bool RequireCompleteCatalogue = false;
		// Delivery mode to record in the exported project.
		engine::game::ProjectDeliveryPreference Delivery = engine::game::ProjectDeliveryPreference::Relay;
		// Whether export output must be byte reproducible.
		bool Reproducible = false;
		// Whether export may replace an existing destination.
		bool ReplaceExisting = false;
	};

	// One normalized export operation passed unchanged through preflight and execution.
	//
	// @since v0.21
	struct ExportRequest {
		// Artifact format accepted by the normalized request.
		engine::game::ExportProduct Product = engine::game::ExportProduct::WorldFile;
		// Destination world or endpoint identifier.
		std::filesystem::path Destination;
		// Whether include processed assets.
		bool IncludeProcessedAssets = false;
		// Whether include raw authoring.
		bool IncludeRawAuthoring = false;
		// Whether include public cdns.
		bool IncludePublicCdns = false;
		// Whether validate cdn configuration.
		bool ValidateCdnConfiguration = true;
		// Whether check remote reachability.
		bool CheckRemoteReachability = false;
		// Whether require complete catalogue.
		bool RequireCompleteCatalogue = false;
		// Delivery mode accepted by the normalized request.
		engine::game::ProjectDeliveryPreference Delivery = engine::game::ProjectDeliveryPreference::Relay;
		// Whether export output must be byte reproducible.
		bool Reproducible = false;
		// Whether export may replace an existing destination.
		bool ReplaceExisting = false;
	};

	// Frozen facts shown before export begins.
	//
	// @since v0.21
	struct ExportPreflight {
		// Fully normalized operation that produced these preflight facts.
		ExportRequest Request;
		// Number of worlds included from local project state.
		uint64_t LocalWorlds = 0;
		// Number of worlds fetched from configured remote sources.
		uint64_t RemoteWorlds = 0;
		// Number of processed assets selected for the archive.
		uint64_t ProcessedAssets = 0;
		// Total byte length of processed asset payloads selected for export.
		uint64_t ProcessedBytes = 0;
		// Estimated uncompressed byte length of the staged export tree.
		uint64_t EstimatedUncompressedBytes = 0;
		// Estimated final archive byte length after packaging.
		uint64_t EstimatedArchiveBytes = 0;
		// Whether publisher key valid.
		bool PublisherKeyValid = false;
		// Whether public http included.
		bool PublicHttpIncluded = false;
		// Delivery sources remaining after product and permission filtering.
		std::vector<engine::delivery::Source> EffectiveSources;
		// Project validation findings collected during preflight.
		engine::game::ProjectValidationReport Validation;
	};

	// Normalizes a destination and rejects choices the selected product cannot carry.
	std::optional<ExportRequest> BuildExportRequest(
		const std::filesystem::path &destination,
		const ExportOptions &options,
		engine::game::ProjectValidationReport &report
	);

	// Inspects the current world, catalogue, sources, permissions, and destination.
	ExportPreflight PreflightExport(
		const ExportRequest &request,
		const engine::world::Universe &universe,
		const ContentSources &content,
		const engine::delivery::AssetClient *client
	);
}
