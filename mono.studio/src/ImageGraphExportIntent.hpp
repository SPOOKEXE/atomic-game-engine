#pragma once

#include "ImageGraphComposerExports.hpp"
#include "ImageGraphExportPreparation.hpp"
#include "ImageGraphExportTriggers.hpp"
#include "ImageGraphHost.hpp"

#include <engine/core/Name.hpp>
#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/imagegraphexport/GraphExportSession.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/imagegraphio/SourceArtworkEdit.hpp>
#include <engine/imagegraphio/SourceImageEdit.hpp>

namespace studio::detail {
	// A pending export owns its observation and cursor. Audio remains caller-owned and
	// may be rebound only after the input generation has been checked.
	struct ImageGraphExportIntent {
		enum class Kind { Authored, HostNode };
		enum class Result { Pending, Complete, Failed };
		struct Target {
			std::string NodeId, Root, ImageEncoder, VideoEncoder;
			std::string ProjectPath{};
			std::optional<engine::imagegraphio::SourceArtworkAction> ArtworkAction{};
			bool MatchRegionNames = true;
			std::optional<engine::imagegraphio::SourceImageAction> ImageAction{};
			bool operator==(const Target &) const = default;
		};
		struct Batch {
			Kind Operation = Kind::Authored;
			engine::core::Name Owner;
			ImageGraphComposerExports::Observation Observation;
			std::optional<engine::imagegraphfont::GraphFontConfiguration> FontConfiguration;
			std::vector<Target> Targets;
			size_t Cursor = 0;
			bool Automatic = false;
			ImageGraphExportEvent Event = ImageGraphExportEvent::Update;
		};
		std::optional<Batch> Current;
		engine::imagegraph::PendingHostObservations Observations;
		ImageGraphExportPreparation Preparation;
		std::vector<engine::core::Name> CaptureNames;
		bool Pending = false;
		static constexpr uint64_t MaximumBytes = 2 * 1024 * 1024;
		// One range reserves owned collection payload separately from compiler,
		// encoder and device workspace. The existing replay owner keeps its own
		// reservation.
		static constexpr uint64_t MaximumRangePayloadBytes = 512ULL * 1024 * 1024;
		static bool AdmitRangePayload(
			std::span<const uint64_t> held,
			uint64_t reservation,
			std::string &failure,
			uint64_t maximumBytes = MaximumRangePayloadBytes
		) {
			uint64_t bytes = reservation;
			if (bytes > maximumBytes) {
				failure = "Range export payload reservation exceeds its byte budget";
				return false;
			}
			for (const uint64_t amount : held) {
				if (amount > maximumBytes - bytes) {
					failure = "Held inputs and range export reservation exceed their byte budget";
					return false;
				}
				bytes += amount;
			}
			return true;
		}

		static std::optional<uint64_t> Bytes(const Batch &batch, uint64_t limit = MaximumBytes) {
			uint64_t bytes = sizeof(batch);
			const auto add = [&](uint64_t count, uint64_t size = 1) {
				if (bytes > limit || count > (limit - bytes) / size) return false;
				bytes += count * size;
				return true;
			};
			const auto observation = ImageGraphComposerExports::Bytes(batch.Observation);
			if (!observation || !add(*observation - sizeof(batch.Observation)) ||
				!add(batch.Targets.capacity(), sizeof(Target)))
				return {};
			for (const auto &target : batch.Targets)
				for (const auto *field :
					 {&target.NodeId,
					  &target.Root,
					  &target.ImageEncoder,
					  &target.VideoEncoder,
					  &target.ProjectPath})
					if (!add(field->capacity()) || !add(1)) return {};
			if (batch.FontConfiguration) {
				const auto fontBytes =
					engine::imagegraphfont::GraphFontConfigurationRetainedBytes(*batch.FontConfiguration);
				if (!fontBytes || *fontBytes < sizeof(*batch.FontConfiguration) ||
					!add(*fontBytes - sizeof(*batch.FontConfiguration)))
					return {};
			}
			return bytes;
		}
		std::optional<uint64_t> MetadataBytes(uint64_t limit = MaximumBytes) const {
			uint64_t bytes = sizeof(*this) - sizeof(Observations);
			if (bytes > limit || CaptureNames.capacity() > (limit - bytes) / sizeof(engine::core::Name))
				return {};
			bytes += CaptureNames.capacity() * sizeof(engine::core::Name);
			if (Current) {
				const auto batch = Bytes(*Current, limit);
				if (!batch || *batch - sizeof(Batch) > limit - bytes) return {};
				bytes += *batch - sizeof(Batch);
			}
			return bytes;
		}
		bool Begin(
			Kind operation,
			engine::core::Name owner,
			const ImageGraphComposerExports::Observation &observation,
			std::span<const Target> targets,
			bool automatic,
			std::string &failure,
			uint64_t maximumBytes = MaximumBytes,
			ImageGraphExportEvent event = ImageGraphExportEvent::Update,
			const engine::imagegraphfont::GraphFontConfiguration *fontConfiguration = nullptr
		) try {
			if (Current || !owner.IsValid() || targets.empty() || targets.size() > 64 ||
				!engine::imagegraph::ValidFrameTime(GetImageGraphFrame(observation.Playback))) {
				failure = "Export intent is active or its owner, targets or frame are invalid";
				return false;
			}
			const auto observationBytes = ImageGraphComposerExports::Bytes(observation);
			const auto heldMetadata = MetadataBytes(maximumBytes);
			const auto heldObservations = Observations.RetainedPayloadBytes();
			if (!heldMetadata || !heldObservations || *heldObservations > maximumBytes - *heldMetadata) {
				failure = "Retained export storage exceeds its byte budget";
				return false;
			}
			const uint64_t held = *heldMetadata + *heldObservations;
			uint64_t source = sizeof(Batch);
			const auto add = [&](uint64_t count, uint64_t size = 1) {
				if (source > maximumBytes || count > (maximumBytes - source) / size) return false;
				source += count * size;
				return true;
			};
			if (!observationBytes || !add(*observationBytes - sizeof(observation)) ||
				!add(targets.size(), sizeof(Target))) {
				failure = "Export intent exceeds its byte budget";
				return false;
			}
			const auto fontBytes =
				fontConfiguration
					? engine::imagegraphfont::GraphFontConfigurationRetainedBytes(*fontConfiguration)
					: std::optional<uint64_t>{};
			if (fontConfiguration && (!fontBytes || *fontBytes < sizeof(*fontConfiguration) ||
									  !add(*fontBytes - sizeof(*fontConfiguration)))) {
				failure = "Frozen font inputs exceed the export intent byte budget";
				return false;
			}
			for (const auto &target : targets) {
				if (target.NodeId.empty() || target.NodeId.size() > 255) {
					failure = "Export target needs a bounded node identity";
					return false;
				}
				for (const auto *field :
					 {&target.NodeId,
					  &target.Root,
					  &target.ImageEncoder,
					  &target.VideoEncoder,
					  &target.ProjectPath})
					if (!add(field->size()) || !add(16)) {
						failure = "Export target metadata exceeds its byte budget";
						return false;
					}
			}
			if (source > (maximumBytes - held) / 2) {
				failure = "Export intent clone exceeds its byte budget";
				return false;
			}
			Batch candidate;
			candidate.Operation = operation;
			candidate.Owner = owner;
			candidate.Observation = observation;
			candidate.Automatic = automatic;
			candidate.Event = event;
			if (fontConfiguration) candidate.FontConfiguration = *fontConfiguration;
			candidate.Targets.reserve(targets.size());
			candidate.Targets.assign(targets.begin(), targets.end());
			const auto cloned = Bytes(candidate, maximumBytes);
			if (!cloned || *cloned > maximumBytes - held - source) {
				failure = "Export intent retained capacity exceeds its byte budget";
				return false;
			}
			Current = std::move(candidate);
			Observations.Clear();
			Pending = false;
			return true;
		} catch (const std::bad_alloc &) {
			failure = "Export intent allocation was refused";
			return false;
		}
		bool Matches(engine::core::Name owner, uint64_t revision, uint64_t inputRevision) const {
			return Current && Current->Owner == owner && Current->Observation.Revision == revision &&
				   Current->Observation.InputRevision == inputRevision;
		}
		const Target *Selected() const {
			return Current && Current->Cursor < Current->Targets.size() ? &Current->Targets[Current->Cursor]
																		: nullptr;
		}
		bool Finish(Result result) {
			if (!Selected()) return false;
			Pending = result == Result::Pending;
			if (Pending) return false;
			++Current->Cursor;
			if (Current->Cursor < Current->Targets.size()) {
				Observations.Clear();
				Preparation.Clear();
			}
			return Current->Cursor == Current->Targets.size();
		}
		void Clear() {
			Preparation.Clear();
			Current.reset();
			Observations.Clear();
			CaptureNames.clear();
			Pending = false;
		}
	};

	// The provider is rebuilt per attempt. Receipts own completed host effects;
	// filesystem grants and renderer ownership are borrowed for this call.
	struct ImageGraphExportProvider final : engine::imagegraphexport::GraphExportSessionHost {
		ImageGraphHost &Host;
		ImageGraphExportIntent &Intent;
		const engine::imagegraph::EvaluationRequest &Request;
		bool CaptureReceipts = true;
		const bool *PendingFlag = nullptr;
		void *CancelContext = nullptr;
		void (*CancelNames)(void *, engine::core::Name, std::span<const engine::core::Name>) noexcept =
			nullptr;
		ImageGraphExportProvider(
			ImageGraphHost &host,
			ImageGraphExportIntent &intent,
			const engine::imagegraph::EvaluationRequest &request
		)
			: Host(host), Intent(intent), Request(request) {}
		bool Pending() const noexcept override {
			return PendingFlag && *PendingFlag;
		}
		void Cancel() noexcept override {
			if (CancelNames && Intent.Current)
				CancelNames(CancelContext, Intent.Current->Owner, Intent.CaptureNames);
			Intent.CaptureNames.clear();
			Intent.Pending = false;
		}
		bool PcxMessages(
			std::string_view node,
			std::span<const engine::imagegraph::PcxMessage> messages,
			std::string &failure
		) override {
			if (!CaptureReceipts) return Host.PcxMessages(node, messages, failure);
			const auto metadata = Intent.Current ? Intent.MetadataBytes() : std::nullopt;
			if (!metadata || *metadata >= engine::imagegraph::Limits::MaximumEvaluationBytes) {
				failure = "Frozen export messages exceed the operation budget";
				return false;
			}
			const auto maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (Host.RetainedObservationBytes() >= maximum - *metadata ||
				Host.LuaReceipts.Bytes >= maximum - *metadata - Host.RetainedObservationBytes()) {
				failure = "Retained Studio observations leave no export message budget";
				return false;
			}
			return Intent.Observations.ForwardMessages(
				Request,
				node,
				messages,
				Host,
				maximum - *metadata - Host.RetainedObservationBytes() - Host.LuaReceipts.Bytes,
				failure
			);
		}
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override {
			const auto metadata = Intent.Current ? Intent.MetadataBytes() : std::nullopt;
			if (!metadata || *metadata >= invocation.MaximumOperationBytes) {
				failure = "Frozen export metadata leaves no host operation budget";
				return false;
			}
			engine::imagegraph::HostNodeInvocation bounded = invocation;
			bounded.MaximumOperationBytes -= *metadata;
			const ImageGraphLuaReceiptScope previewReceipts(Host.LuaReceipts);
			if (CaptureReceipts) return Intent.Observations.CaptureSequenced(bounded, Host, output, failure);
			const auto held = Intent.Observations.RetainedPayloadBytes();
			if (!held || *held >= bounded.MaximumOperationBytes) {
				failure = "Frozen export observations leave no host operation budget";
				return false;
			}
			bounded.MaximumOperationBytes -= *held;
			return Host.Capture(bounded, output, failure);
		}
	};
} // namespace studio::detail
