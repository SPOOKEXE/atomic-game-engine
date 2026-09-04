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

	const char *Describe(ExportPhase phase);

	// Mutable choices collected by the export dialog.
	//
	// @since v0.21
	struct ExportOptions {
		engine::game::ExportProduct Product = engine::game::ExportProduct::WorldFile;
		bool IncludeProcessedAssets = false;
		bool IncludeRawAuthoring = false;
		bool IncludePublicCdns = false;
		bool ValidateCdnConfiguration = true;
		bool CheckRemoteReachability = false;
		bool RequireCompleteCatalogue = false;
		engine::game::ProjectDeliveryPreference Delivery = engine::game::ProjectDeliveryPreference::Relay;
		bool Reproducible = false;
		bool ReplaceExisting = false;
	};

	// One normalized export operation passed unchanged through preflight and execution.
	//
	// @since v0.21
	struct ExportRequest {
		engine::game::ExportProduct Product = engine::game::ExportProduct::WorldFile;
		std::filesystem::path Destination;
		bool IncludeProcessedAssets = false;
		bool IncludeRawAuthoring = false;
		bool IncludePublicCdns = false;
		bool ValidateCdnConfiguration = true;
		bool CheckRemoteReachability = false;
		bool RequireCompleteCatalogue = false;
		engine::game::ProjectDeliveryPreference Delivery = engine::game::ProjectDeliveryPreference::Relay;
		bool Reproducible = false;
		bool ReplaceExisting = false;
	};

	// Frozen facts shown before export begins.
	//
	// @since v0.21
	struct ExportPreflight {
		ExportRequest Request;
		uint64_t LocalWorlds = 0;
		uint64_t RemoteWorlds = 0;
		uint64_t ProcessedAssets = 0;
		uint64_t ProcessedBytes = 0;
		uint64_t EstimatedUncompressedBytes = 0;
		uint64_t EstimatedArchiveBytes = 0;
		bool PublisherKeyValid = false;
		bool PublicHttpIncluded = false;
		std::vector<engine::delivery::Source> EffectiveSources;
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
