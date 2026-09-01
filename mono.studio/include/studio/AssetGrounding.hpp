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
		// Verified catalogue row being copied.
		engine::assets::AssetEntry Entry;

		// Delivery request currently fetching the row.
		engine::delivery::RequestId Request;

		// Whether the processed payload has reached the destination.
		bool Complete = false;
	};

	// The complete state of one incremental grounding operation.
	//
	// @since v0.21
	struct AssetGrounding {
		// Destination `assets/` store root.
		std::filesystem::path Destination;

		// Optional raw root. Empty writes beneath `Destination/raw/`.
		std::filesystem::path RawBaseDestination;

		// Whether the signed processed catalogue is part of this operation.
		bool IncludeProcessed = true;

		// Current incremental operation phase.
		AssetGroundingState State = AssetGroundingState::Idle;

		// Destination chunk store once opened.
		std::optional<engine::assets::ChunkStore> Store;

		// Verified catalogue being grounded.
		std::optional<engine::assets::Manifest> Catalogue;

		// Catalogue signature copied beside the processed store.
		engine::assets::SignatureBytes Signature;

		// Per-asset delivery requests.
		std::vector<GroundedAssetRequest> Requests;

		// Number of completed processed requests.
		size_t Completed = 0;

		// Raw authoring roots copied when requested.
		std::vector<std::filesystem::path> RawSources;

		// Current raw source index.
		size_t RawSource = 0;

		// Current raw source root and destination.
		//@{
		std::filesystem::path RawRoot;
		std::filesystem::path RawDestination;
		//@}

		// Incremental iterator over the current raw source.
		std::filesystem::recursive_directory_iterator RawIterator;

		// Human-readable failure when the state is `Failed`.
		std::string Error;
	};

	// Starts an operation that will write a processed content store.
	//
	// @return False for an empty destination or an operation already in flight.
	bool BeginAssetGrounding(
		AssetGrounding &grounding,
		const std::filesystem::path &destination,
		std::span<const std::filesystem::path> rawSources = {},
		const std::filesystem::path &rawBaseDestination = {},
		bool includeProcessed = true
	);

	// Advances an operation without waiting for pending delivery requests.
	void PumpAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient *client);

	// Convenience overload for a processed operation with a client.
	void PumpAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient &client);

	// Cancels every outstanding request and returns the operation to idle.
	void CancelAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient *client);

	// Convenience overload for an operation with a client.
	void CancelAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient &client);
}
