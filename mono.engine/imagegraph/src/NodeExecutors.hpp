#pragma once

// Native CPU executors for source catalogue nodes.
//
// The evaluator resolves every input before calling an executor: linked image inputs, and for each authored
// input the linked value, junction default, authored value or catalogue default in that order. An executor
// reads those through NodeContext, writes typed outputs, and reports a failure with a durable port name. It
// never sees links, the plan or other nodes.

#include "EvaluationBudget.hpp"
#include "SourceChoice.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/PcxExpression.hpp>
#include <engine/imagegraph/RandomReplay.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/imagegraph/SurfaceFrameReplay.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::imagegraph::detail {
	struct HostCaptureReceiptSink;
	// Evaluation attributes borrow the palette; authored guide storage stays in the document.
	struct EvaluationProjectSettings {
		inline static constexpr Colour DefaultPalette[]{{255, 255, 255, 255}, {0, 0, 0, 255}};
		uint32_t SurfaceWidth = 32, SurfaceHeight = 32;
		int64_t Interpolation = 0, Oversample = 3, ColorDepth = 1;
		std::span<const Colour> Palette = DefaultPalette;
	};
	struct PixelBuilderDrawState {
		uint32_t CirclePrecision = 24;
	};
	class NodeContext {
	  private:
		EvaluationBudget LocalBudget{Limits::MaximumEvaluationBytes};
		EvaluationBudget *Ledger = &LocalBudget;
		AllocationReservation OutputStorageCharge;
		AllocationReservation OutputDiagnosticStorageCharge;
		uint64_t PublishedPayloadBytes = 0;
		bool OutputStorageReady = false;

	  public:
		NodeContext(const Node &authored, const CatalogueEntry &entry, const EvaluationRequest &request)
			: Authored(authored), Entry(entry), Request(request) {}
		NodeContext(
			const Node &authored,
			const CatalogueEntry &entry,
			const EvaluationRequest &request,
			EvaluationBudget &budget
		)
			: Ledger(&budget), Authored(authored), Entry(entry), Request(request) {}

		// Borrowed original views preserve the source array getter clamp bypass during row selection.
		std::span<const std::pair<std::string_view, const Value *>> ProcessorOriginalValues;

		const Node &Authored;
		const CatalogueEntry &Entry;
		const EvaluationRequest &Request;
		// Authored timeline, or null when the document has none.
		const TimelineSettings *Timeline = nullptr;
		// Document project attributes, or the fresh-project defaults.
		EvaluationProjectSettings Project;
		// Null means an unresolved source group Input-format policy.
		std::optional<SurfaceFormat> InheritedSurfaceFormat = SurfaceFormat::RGBA8Unorm;
		int64_t InheritedInterpolation = 1;
		int64_t InheritedOversample = 4;
		// Linked image inputs. An unlinked image input is absent.
		std::vector<std::pair<std::string_view, const Image *>> Images;
		// Array images remain owned by the producing node result.
		std::vector<std::pair<std::string_view, const ImageArray *>> ImageArrays;
		// Resolved authored inputs, one per catalogue input with a document value type.
		std::vector<std::pair<std::string_view, Value>> Values;
		// Temporary processor selections, borrowing bounded row values for one invocation.
		std::vector<std::pair<std::string_view, const Value *>> ValueViews;
		// The executor may copy a bounded recipe; published values never retain this borrowed pointer.
		const Document *EvaluationDocument = nullptr;
		const PcxNameResolver *PcxNames = nullptr;
		AllocationReservation PcxControlCharge;
		std::vector<PcxMessage> PcxControlMessages;
		HostCaptureReceiptSink *HostReceipts = nullptr;
		std::span<const HostNodeCapture> ObservedHostCaptures;
		// Borrowed nearest inline owner's fully resolved inputs, retained by the evaluation.
		std::span<const std::string_view> SimulationColliderIds;
		std::string_view InlineOwnerId;
		std::string_view InlineOwnerType;
		std::span<const std::string_view> InlineOwnerLinkedValues;
		std::span<const std::pair<std::string_view, const Value *>> InlineOwnerValues;
		std::span<const std::pair<std::string_view, const Image *>> InlineOwnerImages;
		std::span<const std::pair<std::string_view, const ImageArray *>> InlineOwnerImageArrays;
		// Source Dimension inputs distinguish resolved links from authored values when applying units.
		std::vector<std::string_view> LinkedValues;
		// Input domains borrow current producer metadata; payload storage remains in its normal owner.
		const GroupReplayEntry *GroupReplay = nullptr;
		std::vector<std::pair<std::string_view, SourceSocketDomain>> InputDomains;
		std::vector<std::pair<std::string_view, SourceSocketDomain>> OutputDomains;
		std::optional<SourceSocketDomain> InputDomain(std::string_view id) const {
			for (const auto &[port, domain] : InputDomains)
				if (port == id) return domain;
			return std::nullopt;
		}
		bool SetOutputDomain(std::string_view id, SourceSocketDomain domain) {
			if (!PrepareOutputStorage(id)) return false;
			for (auto &[port, current] : OutputDomains)
				if (port == id) {
					current = domain;
					return true;
				}
			if (OutputDomains.size() == OutputDomains.capacity())
				return Fail(Status::LimitExceeded, "output domain table exceeds declared slots", id);
			OutputDomains.emplace_back(id, domain);
			return true;
		}
		// Bytes this node may still allocate for outputs.
		uint64_t ByteBudget = 0;
		// Source split determines the bounded output shape before publishing.
		size_t RuntimeOutputCount = 0;

		// Charges precede their buffers so unwinding destroys storage before releasing its admission.
		AllocationReservation OutputCharge;
		std::vector<std::pair<std::string, Image>> OutputImages;
		std::vector<AuthoredValue> OutputValues;
		std::vector<Diagnostic> OutputDiagnostics;
		std::optional<PixelBuilderLayer> PixelBuilderUpdate;
		std::span<const PixelBuilderLayer> PixelBuilderLayers;
		std::optional<Vector2> PixelBuilderCanvas;
		PixelBuilderDrawState *PixelBuilderDrawing = nullptr;
		std::vector<SimulationReplayEntry> SimulationUpdates;
		const SimulationReplayState *CurrentSimulation = nullptr;
		std::span<const SimulationReplayEntry> PendingSimulationRows;
		const SurfaceFrameReplayState *CurrentSurfaces = nullptr;
		const RandomReplayState *CurrentRandom = nullptr;
		const DataReplayState *CurrentData = nullptr;
		std::vector<DataReplayEntry> DataUpdates;
		std::vector<RandomReplayEntry> RandomUpdates;
		std::vector<SurfaceFrameReplayEntry> SurfaceUpdates;
		size_t ProcessorRow = 0;
		size_t ProcessorCount = 1;
		AllocationReservation SimulationAliasCharge;
		std::vector<std::pair<std::string_view, Value>> SimulationAliasValues;

		std::vector<std::pair<std::string, ImageArray>> OutputImageArrays;
		mutable Status FailureCode = Status::Ok;
		mutable std::string FailureMessage;
		mutable std::string FailureNodeId;
		mutable std::string FailurePort;

		EvaluationBudget &AllocationBudget() const {
			return *Ledger;
		}
		uint64_t AvailableBytes() const {
			if (Ledger == &LocalBudget)
				return LocalBudget.Used() > ByteBudget ? 0 : ByteBudget - LocalBudget.Used();
			return std::min(ByteBudget, Ledger->Available());
		}
		[[nodiscard]] std::optional<AllocationReservation>
		ReserveWorkspace(uint64_t bytes, std::string_view port = {}) {
			if (bytes > AvailableBytes()) {
				Fail(Status::LimitExceeded, "live allocation exceeds the evaluation byte budget", port);
				return std::nullopt;
			}
			auto charge = Ledger->Reserve(bytes);
			if (!charge)
				Fail(Status::LimitExceeded, "live allocation exceeds the evaluation byte budget", port);
			return charge;
		}
		[[nodiscard]] bool ReserveOutput(uint64_t bytes, std::string_view port = {}) {
			if (!PrepareOutputStorage(port)) return false;
			auto charge = ReserveWorkspace(bytes, port);
			return charge && OutputCharge.Merge(std::move(*charge));
		}
		void ClearOutputs() {
			OutputImages.clear();
			OutputValues.clear();
			std::vector<Diagnostic>{}.swap(OutputDiagnostics);
			OutputDiagnosticStorageCharge.Reset();
			PixelBuilderUpdate.reset();
			SimulationUpdates.clear();
			SurfaceUpdates.clear();
			RandomUpdates.clear();
			DataUpdates.clear();
			OutputImageArrays.clear();
			OutputDomains.clear();
			PublishedPayloadBytes = 0;
			OutputCharge.Reset();
		}
		// Call after moving all output buffers to the destination owned alongside this reservation.
		AllocationReservation TakeOutputReservation() {
			if (!OutputCharge.Merge(std::move(OutputStorageCharge))) std::terminate();
			if (!OutputCharge.Merge(std::move(OutputDiagnosticStorageCharge))) std::terminate();
			return std::move(OutputCharge);
		}
		// The replacement buffers must already have displaced the old reserved output storage.
		void ReplaceOutputReservation(AllocationReservation &&charge) {
			OutputStorageCharge.Reset();
			OutputDiagnosticStorageCharge.Reset();
			OutputCharge = std::move(charge);
		}

	  private:
		bool PrepareOutputStorage(std::string_view port) {
			if (OutputStorageReady) return true;
			const uint64_t bytes =
				std::max(Entry.Outputs.size() + Authored.DynamicOutputs.size(), RuntimeOutputCount) *
				(sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
				 sizeof(std::pair<std::string, ImageArray>) +
				 sizeof(std::pair<std::string_view, SourceSocketDomain>));
			auto charge = ReserveWorkspace(bytes, port);
			if (!charge) return false;
			OutputStorageCharge = std::move(*charge);
			// NewImage hands out pointers; reserve all declared output slots before publishing any.
			OutputImages.reserve(
				std::max(Entry.Outputs.size() + Authored.DynamicOutputs.size(), RuntimeOutputCount)
			);
			OutputValues.reserve(
				std::max(Entry.Outputs.size() + Authored.DynamicOutputs.size(), RuntimeOutputCount)
			);
			OutputImageArrays.reserve(
				std::max(Entry.Outputs.size() + Authored.DynamicOutputs.size(), RuntimeOutputCount)
			);
			OutputDomains.reserve(
				std::max(Entry.Outputs.size() + Authored.DynamicOutputs.size(), RuntimeOutputCount)
			);
			OutputStorageReady = true;
			return true;
		}
		bool AdmitPublication(uint64_t bytes, std::string_view port) {
			if (!PrepareOutputStorage(port)) return false;
			if (bytes > std::numeric_limits<uint64_t>::max() - PublishedPayloadBytes)
				return Fail(Status::LimitExceeded, "output payload byte count overflow", port);
			const uint64_t total = PublishedPayloadBytes + bytes;
			if (total > OutputCharge.Bytes() && !ReserveOutput(total - OutputCharge.Bytes(), port))
				return false;
			PublishedPayloadBytes = total;
			return true;
		}

	  public:
		const Diagnostic *OutputDiagnostic(std::string_view port) const {
			for (const auto &diagnostic : OutputDiagnostics)
				if (diagnostic.Port == port) return &diagnostic;
			return nullptr;
		}
		// A refused port never substitutes a payload; sibling outputs remain available.
		bool SetOutputDiagnostic(std::string_view port, Status code, std::string_view message) {
			const bool declared = std::any_of(
									  Entry.Outputs.begin(),
									  Entry.Outputs.end(),
									  [&](const auto &output) { return output.Id == port; }
								  ) ||
								  std::any_of(
									  Authored.DynamicOutputs.begin(),
									  Authored.DynamicOutputs.end(),
									  [&](const auto &output) { return output.Id == port; }
								  );
			if (!declared || code != Status::UnsupportedExecution || message.empty())
				return Fail(
					Status::InvalidOutput, "output diagnostic requires a declared refused port", port
				);
			if (port.size() > Limits::MaximumTextBytes || message.size() > Limits::MaximumTextBytes ||
				Authored.Id.size() > Limits::MaximumTextBytes)
				return Fail(Status::LimitExceeded, "output diagnostic text exceeds bounds", port);
			if (const auto *previous = OutputDiagnostic(port))
				return previous->Code == code && previous->Message == message
						   ? true
						   : Fail(
								 Status::InvalidOutput,
								 "output diagnostic conflicts with an earlier refusal",
								 port
							 );
			for (const auto &[id, image] : OutputImages)
				if (id == port)
					return Fail(Status::InvalidOutput, "output diagnostic conflicts with a payload", port);
			for (const auto &[id, images] : OutputImageArrays)
				if (id == port)
					return Fail(Status::InvalidOutput, "output diagnostic conflicts with a payload", port);
			for (const auto &value : OutputValues)
				if (value.Port == port)
					return Fail(Status::InvalidOutput, "output diagnostic conflicts with a payload", port);
			if (!OutputDiagnostics.capacity()) {
				const size_t slots = Entry.Outputs.size() + Authored.DynamicOutputs.size();
				if (slots > Limits::MaximumArrayElements)
					return Fail(Status::LimitExceeded, "output diagnostic slots exceed bounds", port);
				auto charge = ReserveWorkspace(slots * sizeof(Diagnostic), port);
				if (!charge) return false;
				OutputDiagnosticStorageCharge = std::move(*charge);
				OutputDiagnostics.reserve(slots);
			}
			if (OutputDiagnostics.size() == OutputDiagnostics.capacity())
				return Fail(Status::LimitExceeded, "output diagnostic table exceeds declared slots", port);
			const uint64_t strings = std::max(port.size(), std::string{}.capacity()) +
									 std::max(message.size(), std::string{}.capacity()) +
									 std::max(Authored.Id.size(), std::string{}.capacity());
			if (!AdmitPublication(strings, port)) return false;
			OutputDiagnostics.push_back(
				{code, std::string(Authored.Id), std::string(port), std::string(message)}
			);
			return true;
		}
		const Image *Input(std::string_view id) const {
			for (const auto &[port, image] : Images)
				if (port == id) return image;
			return nullptr;
		}

		bool IsLinked(std::string_view id) const {
			for (std::string_view linked : LinkedValues)
				if (linked == id) return true;
			return false;
		}

		// The latest borrowed view wins, then the first owned value. Casts and selected rows override
		// authored inputs.
		const Value *Find(std::string_view id) const {
			for (auto entry = ValueViews.rbegin(); entry != ValueViews.rend(); ++entry)
				if (entry->first == id) return entry->second;
			for (const auto &[port, value] : Values)
				if (port == id) return &value;
			return nullptr;
		}

		// Scalar readers require processor-selected leaves. Executors with manual array semantics
		// use Find; residual runtime arrays diagnose unsupported execution instead of using a fallback.
		template <class T> T Get(std::string_view id, T fallback = T{}) const {
			const Value *value = Find(id);
			if (!value) return fallback;
			if (const auto *typed = std::get_if<T>(value)) return *typed;
			return fallback;
		}
		double Scalar(std::string_view id, double fallback = 0.0) const {
			const Value *value = Find(id);
			if (!value) return fallback;
			if (std::holds_alternative<ArrayValue>(*value)) {
				Fail(Status::UnsupportedExecution, "scalar reader cannot consume an array input", id);
				return fallback;
			}
			if (const auto *scalar = std::get_if<double>(value)) return *scalar;
			if (const auto *integer = std::get_if<int64_t>(value)) return static_cast<double>(*integer);
			return fallback;
		}
		double SourceChoice(std::string_view id, double fallback = 0) const {
			const Value *value = Find(id);
			const auto number = value ? SourceChoiceNumber(*value) : std::optional<double>(fallback);
			if (!number) {
				Fail(Status::UnsupportedExecution, "source choice requires a finite numeric value", id);
				return fallback;
			}
			const CatalogueInput *input = FindCatalogueInput(Entry, id);
			if (!input) {
				size_t group = 0;
				input = FindDynamicTemplate(Entry, id, group);
			}
			if (!input || !input->SourceBehavior) return *number;
			if (std::trunc(*number) != *number && input->SourceBehavior->FractionalInterpolation != true) {
				Fail(Status::UnsupportedExecution, "fractional source choice behavior is unknown", id);
				return fallback;
			}
			bool arraySelection = false, originalFound = false;
			for (auto entry = ProcessorOriginalValues.rbegin(); entry != ProcessorOriginalValues.rend();
				 ++entry)
				if (entry->first == id) {
					originalFound = true;
					arraySelection = std::holds_alternative<ArrayValue>(*entry->second);
					break;
				}
			if (!originalFound)
				for (const auto &[port, original] : Values)
					if (port == id && std::holds_alternative<ArrayValue>(original)) arraySelection = true;
			const auto normalized = NormalizeSourceChoice(*input, *number, arraySelection);
			if (!normalized) {
				Fail(Status::UnsupportedExecution, "source choice clamp policy or range is unknown", id);
				return fallback;
			}
			return *normalized;
		}
		int64_t Integer(std::string_view id, int64_t fallback = 0) const {
			const Value *value = Find(id);
			if (value && std::holds_alternative<ArrayValue>(*value)) {
				Fail(Status::UnsupportedExecution, "integer reader cannot consume an array input", id);
				return fallback;
			}
			const CatalogueInput *source = FindCatalogueInput(Entry, id);
			if (!source) {
				size_t group = 0;
				source = FindDynamicTemplate(Entry, id, group);
			}
			if (source && source->Type == ValueType::Enum && source->SourceBehavior) {
				const double choice = SourceChoice(id, static_cast<double>(fallback));
				if (FailureCode != Status::Ok) return fallback;
				if (std::trunc(choice) != choice) {
					Fail(
						Status::UnsupportedExecution,
						"fractional source choice is not supported by this executor",
						id
					);
					return fallback;
				}
				if (choice < -0x1p63 || choice >= 0x1p63) {
					Fail(Status::InvalidValue, "source choice exceeds integer range", id);
					return fallback;
				}
				return static_cast<int64_t>(choice);
			}
			if (!value) return fallback;
			if (const auto *integer = std::get_if<int64_t>(value)) return *integer;
			if (const auto *choice = std::get_if<EnumValue>(value)) return choice->Value;
			if (const auto *scalar = std::get_if<double>(value)) {
				// The positive int64 limit rounds up as a double, so it is an exclusive bound.
				if (!std::isfinite(*scalar) || *scalar < -0x1p63 || *scalar >= 0x1p63) {
					Fail(Status::InvalidValue, "integer input must be finite and within int64 range", id);
					return fallback;
				}
				return static_cast<int64_t>(*scalar);
			}
			return fallback;
		}
		bool Boolean(std::string_view id, bool fallback = false) const {
			const Value *value = Find(id);
			if (!value) return fallback;
			if (std::holds_alternative<ArrayValue>(*value)) {
				Fail(Status::UnsupportedExecution, "boolean reader cannot consume an array input", id);
				return fallback;
			}
			if (const auto *flag = std::get_if<bool>(value)) return *flag;
			if (const auto *integer = std::get_if<int64_t>(value)) return *integer != 0;
			if (const auto *scalar = std::get_if<double>(value)) return *scalar != 0.0;
			return fallback;
		}
		// Linked source values are untyped, so a number reads as a vector with both components equal.
		Vector2 Vec2(std::string_view id, Vector2 fallback = {}) const {
			const Value *value = Find(id);
			if (!value) return fallback;
			if (const auto *vector = std::get_if<Vector2>(value)) return *vector;
			if (const auto *scalar = std::get_if<double>(value)) return {*scalar, *scalar};
			if (const auto *integer = std::get_if<int64_t>(value))
				return {static_cast<double>(*integer), static_cast<double>(*integer)};
			return fallback;
		}

		// Checks caller dimensions and the node byte budget before allocating a declared image output.
		Image *NewImage(
			std::string_view id,
			uint32_t width,
			uint32_t height,
			SurfaceFormat format = SurfaceFormat::RGBA8Unorm
		) {
			if (OutputDiagnostic(id)) {
				Fail(Status::InvalidOutput, "output payload conflicts with a refusal", id);
				return nullptr;
			}
			if (!PrepareOutputStorage(id)) return nullptr;
			if (Request.MaximumImageDimension == 0 ||
				Request.MaximumImageDimension > Limits::MaximumDimension) {
				Fail(
					Status::InvalidValue, "request image dimension budget is outside the supported range", id
				);
				return nullptr;
			}
			if (width > Request.MaximumImageDimension || height > Request.MaximumImageDimension) {
				Fail(Status::LimitExceeded, "output exceeds the request image dimension budget", id);
				return nullptr;
			}
			if (OutputImages.size() == OutputImages.capacity()) {
				Fail(Status::InvalidOutput, "executor wrote more outputs than the node declares", id);
				return nullptr;
			}
			if (width == 0 || height == 0 || width > Limits::MaximumDimension ||
				height > Limits::MaximumDimension) {
				Fail(Status::LimitExceeded, "output dimensions are outside the supported range", id);
				return nullptr;
			}
			const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumOutputBytes);
			if (!layout) {
				Fail(Status::LimitExceeded, "output exceeds the byte budget", id);
				return nullptr;
			}
			const uint64_t bytes = layout->Bytes;
			if (!AdmitPublication(bytes + std::max(id.size(), std::string{}.capacity()), id)) return nullptr;
			Image image;
			image.Width = width;
			image.Height = height;
			image.Format = format;
			{
				ENGINE_PROFILE("imagegraph.image.allocate");
				image.Pixels.assign(static_cast<size_t>(bytes), 0);
				core::Metrics::Count("imagegraph.image.allocated_payload_bytes", image.Pixels.size());
				core::Metrics::Count("imagegraph.image.allocations", 1);
			}
			OutputImages.emplace_back(std::string(id), std::move(image));
			return &OutputImages.back().second;
		}

		template <class T> void SetValue(std::string_view id, T &&value) {
			if (OutputDiagnostic(id)) {
				Fail(Status::InvalidOutput, "output payload conflicts with a refusal", id);
				return;
			}
			const uint64_t owned = RetainedPayloadBytes(value);
			if (!AdmitPublication(owned + std::max(id.size(), std::string{}.capacity()), id)) return;
			if (OutputValues.size() == OutputValues.capacity()) {
				Fail(Status::InvalidOutput, "executor wrote more outputs than the node declares", id);
				return;
			}
			OutputValues.push_back({std::string(id), Value{std::forward<T>(value)}});
			// Payload size is publication work, not an allocator commitment or allocation count.
			core::Metrics::Count(
				"imagegraph.value.output_payload_bytes", ValuePayloadBytes(OutputValues.back().Data)
			);
			core::Metrics::Count("imagegraph.value.outputs", 1);
		}

		// Preserve producer identity when an indirect input reader returns a diagnostic.
		bool Fail(const Diagnostic &diagnostic, std::string_view fallbackPort = {}) const {
			if (FailureCode != Status::Ok) return false;
			FailureNodeId = diagnostic.NodeId;
			return Fail(
				diagnostic.Code,
				diagnostic.Message,
				diagnostic.Port.empty() ? fallbackPort : std::string_view{diagnostic.Port}
			);
		}
		bool Fail(Status code, std::string message, std::string_view port = {}) const {
			if (FailureCode != Status::Ok) return false;
			FailureCode = code;
			FailureMessage = std::move(message);
			FailurePort = std::string(port);
			return false;
		}
	};

	// Returns false after calling NodeContext::Fail.
	using Executor = bool (*)(NodeContext &context);

	struct ExecutorEntry {
		std::string_view Type;
		Executor Run;
		// True only after all input, output and finish paths preserve numeric surface formats.
		bool TypedSurfaces = false;
	};

	// Every registered executor, keyed by catalogue type.
	Executor FindExecutor(std::string_view type);
	// Registered executor types, for coverage checks.
	std::vector<std::string_view> ExecutorTypes();
}
