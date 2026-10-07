#pragma once

// Retained processing context for one source action. Combine samples its getter at
// each key clock while self/node_values keeps this immutable action map.
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceAxisTransition.hpp>
#include <engine/imagegraph/SourceInputProcessingObserver.hpp>

#include <algorithm>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace studio::detail {
	struct ImageGraphAxisObservationIdentity {
		std::string NodeId, OwnerId;
		uint64_t AuthoringRevision = 0, InputRevision = 0;
		engine::imagegraph::FrameTime Frame;
		uint64_t GroupRevision = 0;
		bool operator==(const ImageGraphAxisObservationIdentity &) const = default;
	};
	struct ImageGraphAxisObservation {
		std::optional<ImageGraphAxisObservationIdentity> Identity;
		std::vector<engine::imagegraph::AuthoredValue> Inputs;
		std::optional<uint64_t> RetainedBytes() const {
			using namespace engine::imagegraph;
			uint64_t bytes = sizeof(ImageGraphAxisObservation) + Inputs.capacity() * sizeof(AuthoredValue);
			if (Identity) bytes += Identity->NodeId.capacity() + Identity->OwnerId.capacity() + 2;
			for (const auto &input : Inputs) {
				const auto payload = ValueClonePayloadBytes(input.Data);
				if (!payload || *payload > Limits::MaximumEvaluationBytes ||
					bytes > Limits::MaximumEvaluationBytes - *payload)
					return std::nullopt;
				bytes += *payload;
				const uint64_t name = input.Port.capacity() + 1;
				if (name > Limits::MaximumEvaluationBytes - bytes) return std::nullopt;
				bytes += name;
			}
			return bytes <= Limits::MaximumEvaluationBytes ? std::optional<uint64_t>{bytes} : std::nullopt;
		}
	};
	// Called with actual source-processing inputs, never with a replacement action-time evaluation.
	inline bool RetainImageGraphAxisObservation(
		const ImageGraphAxisObservationIdentity &identity,
		std::span<const engine::imagegraph::AuthoredValue> processedInputs,
		ImageGraphAxisObservation &observation,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine::imagegraph;
		const auto refuse = [&](Status status, const char *message) {
			error = {status, {}, {}, message};
			return false;
		};
		if (identity.NodeId.empty() || identity.OwnerId.empty() || !ValidFrameTime(identity.Frame))
			return refuse(Status::InvalidValue, "source axis processing observation identity is invalid");
		if (identity.NodeId.size() > Limits::MaximumTextBytes ||
			identity.OwnerId.size() > Limits::MaximumTextBytes ||
			processedInputs.size() > Limits::MaximumArrayElements)
			return refuse(
				Status::LimitExceeded, "source axis processing observation exceeds identity bounds"
			);
		const auto held = observation.RetainedBytes();
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (!held || *held > maximumBytes)
			return refuse(Status::LimitExceeded, "retained source axis observation exceeds payload bounds");
		uint64_t remaining = maximumBytes - *held;
		const auto spend = [&](uint64_t bytes) {
			if (bytes > remaining) return false;
			remaining -= bytes;
			return true;
		};
		if (!spend(
				sizeof(ImageGraphAxisObservation) +
				std::max(identity.NodeId.size(), std::string{}.capacity()) +
				std::max(identity.OwnerId.size(), std::string{}.capacity()) + 2
			) ||
			!spend(processedInputs.size() * sizeof(AuthoredValue)))
			return refuse(Status::LimitExceeded, "source axis observation storage exceeds payload bounds");
		uint64_t work = 0;
		for (size_t index = 0; index < processedInputs.size(); ++index) {
			const auto &input = processedInputs[index];
			const auto payload = ValueClonePayloadBytes(input.Data);
			if (input.Port.empty() || input.Port.size() > Limits::MaximumTextBytes || !payload ||
				!spend(*payload) || !spend(std::max(input.Port.size(), std::string{}.capacity()) + 1))
				return refuse(Status::LimitExceeded, "source axis input observation exceeds payload bounds");
			for (size_t prior = 0; prior < index; ++prior) {
				const uint64_t comparison =
					1 + std::min(input.Port.size(), processedInputs[prior].Port.size());
				if (comparison > 64'000'000 - work)
					return refuse(Status::LimitExceeded, "source axis input map lookup exceeds work bounds");
				work += comparison;
				if (input.Port == processedInputs[prior].Port)
					return refuse(
						Status::InvalidValue, "source axis input observation contains duplicate ports"
					);
			}
		}
		ImageGraphAxisObservation candidate;
		candidate.Identity = identity;
		candidate.Inputs.reserve(processedInputs.size());
		for (const auto &input : processedInputs)
			candidate.Inputs.push_back(input);
		const auto actual = candidate.RetainedBytes();
		if (!actual || *actual > maximumBytes - *held)
			return refuse(Status::LimitExceeded, "source axis observation capacity exceeds payload bounds");
		observation = std::move(candidate);
		error = {};
		return true;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded, {}, {}, "source axis observation allocation failed"
		};
		return false;
	}
	// A preview owns this candidate. Publish it only after the complete preview and identity checks succeed.
	struct ImageGraphAxisProcessingObserver final : engine::imagegraph::SourceInputProcessingObserver {
		ImageGraphAxisObservationIdentity Expected;
		ImageGraphAxisObservation Candidate;
		uint64_t RemainingWork = 64'000'000;
		bool Processed = false;
		engine::imagegraph::Status Failure = engine::imagegraph::Status::Ok;
		explicit ImageGraphAxisProcessingObserver(ImageGraphAxisObservationIdentity identity)
			: Expected(std::move(identity)) {}
		std::string_view NodeId() const noexcept override {
			return Expected.OwnerId;
		}
		bool ObservesFrame(engine::imagegraph::FrameTime frame) const noexcept override {
			return frame == Expected.Frame;
		}
		uint64_t RetainedBytes() const noexcept override {
			const auto candidate = Candidate.RetainedBytes();
			const uint64_t identity = sizeof(ImageGraphAxisProcessingObserver) + Expected.NodeId.capacity() +
									  Expected.OwnerId.capacity() + 2;
			if (!candidate || identity > engine::imagegraph::Limits::MaximumEvaluationBytes - *candidate)
				return engine::imagegraph::Limits::MaximumEvaluationBytes;
			return *candidate + identity;
		}
		engine::imagegraph::Status Observe(
			engine::imagegraph::FrameTime frame,
			std::span<const engine::imagegraph::EvaluationInputValue> values,
			std::span<const engine::imagegraph::EvaluationInputImage> images,
			std::span<const engine::imagegraph::SnapshotImageArray> arrays,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes
		) override try {
			using namespace engine::imagegraph;
			// Stateful seeks also process earlier clocks. Only the requested final clock is retained.
			if (frame != Expected.Frame) return Status::Ok;
			uint64_t bytes = (values.size() + images.size() + arrays.size()) * sizeof(AuthoredValue);
			const auto spend = [&](uint64_t size) {
				if (size > maximumBytes - std::min(bytes, maximumBytes)) return false;
				bytes += size;
				return true;
			};
			for (const auto &value : values) {
				const auto size = ValueClonePayloadBytes(value.Data);
				if (!size || !spend(*size + value.Port.capacity() + 1)) goto limit;
			}
			for (const auto &image : images)
				if (!spend(sizeof(SurfaceValue) + image.Data.Pixels.size() + image.Port.capacity() + 1))
					goto limit;
			// Admit every repeated leaf before copying, preserving the source array's nested shape.
			for (const auto &array : arrays) {
				if (!spend(sizeof(ArrayValue) + array.Port.capacity() + 1)) goto limit;
				const auto admit = [&](const auto &self, const ImageArrayItem &item, uint32_t depth) -> bool {
					if (depth > 32 || !spend(sizeof(SourceArrayItem))) return false;
					if (const auto *index = std::get_if<size_t>(&item.Data))
						return *index < array.Data.Images.size() &&
							   spend(array.Data.Images[*index].Pixels.size());
					for (const auto &child : std::get<std::vector<ImageArrayItem>>(item.Data))
						if (!self(self, child, depth + 1)) return false;
					return true;
				};
				if (array.Data.Items.empty()) {
					for (const auto &image : array.Data.Images)
						if (!spend(sizeof(SourceArrayItem) + image.Pixels.size())) goto limit;
				} else
					for (const auto &item : array.Data.Items)
						if (!admit(admit, item, 1)) goto limit;
			}
			if (bytes > RemainingWork / 3) goto limit;
			RemainingWork -= bytes * 3;
			{
				std::vector<AuthoredValue> inputs;
				inputs.reserve(values.size() + images.size() + arrays.size());
				for (const auto &value : values)
					inputs.push_back({value.Port, value.Data});
				for (const auto &image : images)
					inputs.push_back({image.Port, SurfaceValue{image.Data}});
				for (const auto &array : arrays) {
					ArrayValue result;
					result.ElementType = ValueType::Any;
					result.Items.reserve(
						array.Data.Items.empty() ? array.Data.Images.size() : array.Data.Items.size()
					);
					const auto copy = [&](const auto &self, const ImageArrayItem &item) -> SourceArrayItem {
						if (const auto *index = std::get_if<size_t>(&item.Data))
							return {array.Data.Images[*index]};
						std::vector<SourceArrayItem> children;
						children.reserve(std::get<std::vector<ImageArrayItem>>(item.Data).size());
						for (const auto &child : std::get<std::vector<ImageArrayItem>>(item.Data))
							children.push_back(self(self, child));
						return {std::move(children)};
					};
					if (array.Data.Items.empty())
						for (const auto &image : array.Data.Images)
							result.Items.push_back({image});
					else
						for (const auto &item : array.Data.Items)
							result.Items.push_back(copy(copy, item));
					inputs.push_back({array.Port, std::move(result)});
				}
				return RetainImageGraphAxisObservation(
						   Expected, inputs, Candidate, error, maximumBytes - bytes
					   )
						   ? Status::Ok
						   : error.Code;
			}
		limit:
			error = {
				Status::LimitExceeded, {}, {}, "source processing receipt conversion exceeds payload bounds"
			};
			return error.Code;
		} catch (const std::bad_alloc &) {
			error = {
				engine::imagegraph::Status::LimitExceeded,
				{},
				{},
				"source processing receipt allocation failed"
			};
			return error.Code;
		}
	};

	// Only expression owners of the selected node's actionable axes are requested.
	struct ImageGraphAxisProcessingObservers final : engine::imagegraph::SourceInputProcessingObserver {
		std::vector<ImageGraphAxisProcessingObserver> Owners;
		uint64_t RemainingWork = 64'000'000;
		uint64_t MaximumReceiptBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
		engine::imagegraph::Status Failure = engine::imagegraph::Status::Ok;
		std::string_view NodeId() const noexcept override {
			return {};
		}
		bool ObservesFrame(engine::imagegraph::FrameTime frame) const noexcept override {
			return !Owners.empty() && frame == Owners.front().Expected.Frame;
		}
		bool ObservesNode(std::string_view nodeId) const noexcept override {
			for (const auto &owner : Owners)
				if (owner.Expected.OwnerId == nodeId) return true;
			return false;
		}
		uint64_t RetainedBytes() const noexcept override {
			if (Owners.empty()) return 0;
			uint64_t bytes = sizeof(*this) + Owners.capacity() * sizeof(ImageGraphAxisProcessingObserver);
			for (const auto &owner : Owners) {
				const auto held = owner.RetainedBytes();
				if (held > engine::imagegraph::Limits::MaximumEvaluationBytes -
							   std::min(bytes, engine::imagegraph::Limits::MaximumEvaluationBytes))
					return engine::imagegraph::Limits::MaximumEvaluationBytes;
				bytes += held;
			}
			return bytes;
		}
		engine::imagegraph::Status Observe(
			engine::imagegraph::FrameTime,
			std::span<const engine::imagegraph::EvaluationInputValue>,
			std::span<const engine::imagegraph::EvaluationInputImage>,
			std::span<const engine::imagegraph::SnapshotImageArray>,
			engine::imagegraph::Diagnostic &error,
			uint64_t
		) override {
			error = {engine::imagegraph::Status::InvalidValue, {}, {}, "source owner identity is required"};
			return error.Code;
		}
		engine::imagegraph::Status ObserveNode(
			std::string_view nodeId,
			engine::imagegraph::FrameTime frame,
			std::span<const engine::imagegraph::EvaluationInputValue> values,
			std::span<const engine::imagegraph::EvaluationInputImage> images,
			std::span<const engine::imagegraph::SnapshotImageArray> arrays,
			engine::imagegraph::Diagnostic &error,
			uint64_t maximumBytes
		) override {
			const uint64_t lookup = Owners.size() * (nodeId.size() + 1);
			if (lookup > RemainingWork) {
				error = {
					engine::imagegraph::Status::LimitExceeded,
					{},
					{},
					"source axis owner receipt lookup exceeds work bounds"
				};
				return CaptureRefused(nodeId, error);
			}
			RemainingWork -= lookup;
			for (auto &owner : Owners)
				if (owner.Expected.OwnerId == nodeId) {
					if (frame != owner.Expected.Frame) return engine::imagegraph::Status::Ok;
					owner.Processed = true;
					const auto work = std::min(RemainingWork, owner.RemainingWork);
					owner.RemainingWork = work;
					const auto status = owner.Observe(
						frame, values, images, arrays, error, std::min(maximumBytes, MaximumReceiptBytes)
					);
					RemainingWork -= work - owner.RemainingWork;
					if (status != engine::imagegraph::Status::Ok) return CaptureRefused(nodeId, error);
					owner.Failure = engine::imagegraph::Status::Ok;
					return status;
				}
			return Observe(frame, values, images, arrays, error, maximumBytes);
		}
		engine::imagegraph::Status
		CaptureRefused(std::string_view nodeId, engine::imagegraph::Diagnostic &error) override {
			if (nodeId.empty()) {
				Failure = error.Code;
				std::vector<ImageGraphAxisProcessingObserver>{}.swap(Owners);
				error = {};
				return engine::imagegraph::Status::Ok;
			}
			for (auto &owner : Owners)
				if (owner.Expected.OwnerId == nodeId) {
					owner.Candidate = {};
					owner.Failure = error.Code;
					owner.Processed = true;
					error = {};
					return engine::imagegraph::Status::Ok;
				}
			return error.Code;
		}
		engine::imagegraph::Status FailureFor(const ImageGraphAxisObservationIdentity &identity) const {
			for (const auto &owner : Owners)
				if (owner.Expected == identity) return owner.Failure;
			return Failure;
		}

		const ImageGraphAxisObservation *Find(const ImageGraphAxisObservationIdentity &identity) const {
			for (const auto &owner : Owners)
				if (owner.Candidate.Identity && *owner.Candidate.Identity == identity)
					return &owner.Candidate;
			return nullptr;
		}
	};
	inline void PublishImageGraphAxisProcessingObservers(
		ImageGraphAxisProcessingObservers &retained,
		ImageGraphAxisProcessingObservers candidate,
		bool complete,
		std::string_view nodeId,
		uint64_t revision,
		uint64_t inputRevision,
		engine::imagegraph::FrameTime frame
	) {
		if (!complete) {
			retained = {};
			return;
		}
		for (auto &owner : candidate.Owners) {
			if (owner.Expected.NodeId != nodeId || owner.Expected.AuthoringRevision != revision ||
				owner.Expected.InputRevision != inputRevision || owner.Expected.Frame != frame) {
				retained = {};
				return;
			}
			if (!owner.Processed && !owner.Candidate.Identity)
				for (auto &old : retained.Owners)
					if (old.Expected == owner.Expected) {
						owner.Candidate = std::move(old.Candidate);
						break;
					}
		}
		retained = std::move(candidate);
	}

	inline bool PrepareImageGraphAxisProcessingObservers(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::GroupReplayState &replay,
		std::string_view nodeId,
		uint64_t revision,
		uint64_t inputRevision,
		engine::imagegraph::FrameTime frame,
		ImageGraphAxisProcessingObservers &result,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine::imagegraph;
		ImageGraphAxisProcessingObservers candidate;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
			return item.Id == nodeId;
		});
		if (node == document.Nodes.end()) {
			result = std::move(candidate);
			return true;
		}
		const auto *schema = FindSchema(node->Type);
		if (!schema) {
			result = std::move(candidate);
			return true;
		}
		uint64_t work = 64'000'000;
		const auto append = [&](std::string_view port) {
			if (!SupportsSourceAxisTransition(*node, port)) return true;
			const uint64_t lookup = document.Nodes.size() * document.Nodes.size() + candidate.Owners.size();
			if (lookup > work) return false;
			work -= lookup;
			const auto owner = SourceInputExpressionOwner(document, plan, nodeId, port, &replay);
			if (!owner) return false;
			if (candidate.ObservesNode(*owner)) return true;
			const uint64_t next =
				sizeof(ImageGraphAxisProcessingObserver) * 2 + nodeId.size() + owner->size() + 64;
			if (next > maximumBytes - std::min(candidate.RetainedBytes(), maximumBytes)) return false;
			candidate.Owners.emplace_back(
				ImageGraphAxisObservationIdentity{
					std::string(nodeId),
					std::string(*owner),
					revision,
					inputRevision,
					frame,
					replay.ObservationRevision()
				}
			);
			return candidate.RetainedBytes() <= maximumBytes;
		};
		for (const auto &port : schema->Ports)
			if (port.Direction == PortDirection::Input && !append(port.Id)) goto refuse;
		for (const auto &port : node->DynamicInputs)
			if (!append(port.Id)) goto refuse;
		result = std::move(candidate);
		error = {};
		return true;
	refuse:
		error = {Status::LimitExceeded, {}, {}, "source axis owner set exceeds receipt bounds"};
		return false;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded, {}, {}, "source axis owner set allocation failed"
		};
		return false;
	}

	inline bool BindImageGraphAxisObservation(
		const ImageGraphAxisObservation &observation,
		const ImageGraphAxisObservationIdentity &current,
		engine::imagegraph::SourceAxisTransition &transition,
		engine::imagegraph::Diagnostic &error
	) {
		using namespace engine::imagegraph;
		if (!observation.Identity || *observation.Identity != current ||
			transition.NodeId != current.NodeId || !observation.RetainedBytes()) {
			error = {
				Status::InvalidValue,
				{},
				{},
				"source axis action requires matching retained processing inputs"
			};
			return false;
		}
		transition.ObservedInputs = observation.Inputs;
		transition.ObservedInputOwner = observation.Identity->OwnerId;
		error = {};
		return true;
	}
}
