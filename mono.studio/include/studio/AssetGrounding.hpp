#pragma once

// Incrementally grounds a verified delivery catalogue into a local processed
// store. It fetches no raw authoring inputs and never blocks a frame waiting
// for a source.

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/delivery/Client.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace studio {

	// Where a grounding operation has reached.
	//
	// @since v0.21
	enum class AssetGroundingState : uint8_t {
		Idle,
		WaitingForCatalogue,
		Fetching,
		CopyingRaw,
		Complete,
		Failed
	};

	// One catalogue entry and the delivery request filling it.
	//
	// @since v0.21
	struct GroundedAssetRequest {
		engine::assets::AssetEntry Entry;
		engine::delivery::RequestId Request;
		bool Complete = false;
	};

	// The complete state of one incremental grounding operation.
	//
	// @since v0.21
	struct AssetGrounding {
		std::filesystem::path Destination;
		AssetGroundingState State = AssetGroundingState::Idle;
		std::optional<engine::assets::ChunkStore> Store;
		std::optional<engine::assets::Manifest> Catalogue;
		engine::assets::SignatureBytes Signature;
		std::vector<GroundedAssetRequest> Requests;
		size_t Completed = 0;
		std::vector<std::filesystem::path> RawSources;
		size_t RawSource = 0;
		std::filesystem::path RawRoot;
		std::filesystem::path RawDestination;
		std::filesystem::recursive_directory_iterator RawIterator;
		std::string Error;
	};

	// Starts an operation that will write a processed content store.
	//
	// @return False for an empty destination or an operation already in flight.
	bool BeginAssetGrounding(
		AssetGrounding &grounding,
		const std::filesystem::path &destination,
		std::span<const std::filesystem::path> rawSources = {}
	);

	// Advances an operation without waiting for pending delivery requests.
	void PumpAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient &client);

	// Cancels every outstanding request and returns the operation to idle.
	void CancelAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient &client);
}
