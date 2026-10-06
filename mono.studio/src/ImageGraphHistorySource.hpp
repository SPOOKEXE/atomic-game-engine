#pragma once

#include <engine/imagegraphio/PxcxImport.hpp>

#include <new>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	inline std::optional<uint64_t>
	ImageGraphHistoryDiagnosticBytes(const std::vector<engine::imagegraph::Diagnostic> &items) {
		const auto maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
		if (items.capacity() > maximum / sizeof(engine::imagegraph::Diagnostic)) return {};
		uint64_t bytes = items.capacity() * sizeof(engine::imagegraph::Diagnostic);
		for (const auto &item : items)
			for (const auto *text : {&item.NodeId, &item.Port, &item.Message}) {
				if (text->capacity() > maximum - bytes) return {};
				bytes += text->capacity();
			}
		return bytes;
	}
	inline std::optional<uint64_t> ImageGraphHistoryArchiveBytes(const engine::bake::PxcxArchive &archive) {
		using engine::bake::PxcxLimits;
		if (archive.Nodes.size() > PxcxLimits::MaximumNodes ||
			archive.Links.size() > PxcxLimits::MaximumLinks)
			return {};
		uint64_t bytes = sizeof(archive);
		const auto charge = [&](uint64_t count, uint64_t width = 1) {
			const auto maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (count && width > (maximum - bytes) / count) return false;
			bytes += count * width;
			return true;
		};
		if (!charge(archive.OriginalBytes.capacity()) || !charge(archive.ThumbnailRgba.capacity()) ||
			!charge(archive.MetadataPayload.capacity()) || !charge(archive.MetadataText.capacity()) ||
			!charge(archive.GraphJson.capacity()) ||
			!charge(archive.Nodes.capacity(), sizeof(engine::bake::PxcxNodeFact)) ||
			!charge(archive.Links.capacity(), sizeof(engine::bake::PxcxLinkFact)))
			return {};
		for (const auto &node : archive.Nodes)
			if (!charge(node.Id.capacity()) || !charge(node.Type.capacity())) return {};
		for (const auto &link : archive.Links)
			if (!charge(link.FromNode.capacity()) || !charge(link.ToNode.capacity())) return {};
		return bytes;
	}

	// grug decode a changed source epoch privately. native undo keeps its unsaved authored document.
	struct ImageGraphHistorySource {
		std::optional<engine::bake::PxcxArchive> Archive;
		engine::imagegraph::Document Projection;
		std::vector<engine::imagegraph::Diagnostic> Diagnostics;
		engine::imagegraph::Image Thumbnail;
		bool Changed = false;

		bool Prepare(
			const ImageGraphHistory::SourceSnapshot &current,
			const ImageGraphHistory::SourceSnapshot &target,
			const std::optional<engine::bake::PxcxArchive> &live,
			uint64_t &remaining,
			engine::imagegraph::Diagnostic &diagnostic
		) try {
			using namespace engine::imagegraph;
			const auto fail = [&](Status code, const char *message) {
				diagnostic = {code, {}, {}, message};
				return false;
			};
			if (Changed) return fail(Status::InvalidValue, "Source history candidate was already prepared");
			if (!remaining || remaining > Limits::MaximumEvaluationBytes)
				return fail(Status::LimitExceeded, "Source history allowance is outside bounds");
			if (current == target) return true;
			const auto priorArchive =
				Archive ? ImageGraphHistoryArchiveBytes(*Archive) : std::optional<uint64_t>{0};
			const auto priorProjection = DocumentRetainedPayloadBytes(Projection);
			const auto priorDiagnostics = ImageGraphHistoryDiagnosticBytes(Diagnostics);
			uint64_t available = remaining;
			for (const auto bytes :
				 {priorArchive,
				  priorProjection,
				  priorDiagnostics,
				  std::optional<uint64_t>{Thumbnail.Pixels.capacity()}}) {
				if (!bytes || *bytes >= available)
					return fail(
						Status::LimitExceeded, "Source history destination overlap exceeds live bytes"
					);
				available -= *bytes;
			}
			if (!current || !target || !live || live->OriginalBytes != *current)
				return fail(Status::InvalidValue, "Source history baseline differs from its live archive");
			if (target->empty() || target->size() > engine::bake::PxcxLimits::MaximumArchiveBytes ||
				target->capacity() > available / 2)
				return fail(Status::LimitExceeded, "Source history archive leaves no decode allowance");
			engine::bake::PxcxArchive decoded;
			std::string error;
			if (!engine::bake::ReadPxcx(*target, decoded, error)) {
				diagnostic = {Status::InvalidValue, {}, {}, std::move(error)};
				return false;
			}
			const auto decodedBytes = ImageGraphHistoryArchiveBytes(decoded);
			if (!decodedBytes || *decodedBytes >= available)
				return fail(Status::LimitExceeded, "Decoded source history archive exceeds live bytes");
			engine::imagegraphio::PxcxImport imported;
			engine::imagegraphio::PxcxImportOptions options;
			options.MaximumOperationBytes = available - *decodedBytes;
			if (!engine::imagegraphio::ImportPxcxImageGraph(decoded, imported, error, options)) {
				diagnostic = {Status::InvalidValue, {}, {}, std::move(error)};
				return false;
			}
			if (Migrate(imported.Graph, diagnostic) != Status::Ok) return false;
			// grug drop decode and append-only storage before reserving the thumbnail copy.
			decoded = {};
			imported.GroupPrebinding.reset();
			imported.GroupBootstrap = std::vector<engine::imagegraphio::PxcxGroupBootstrapRecord>{};
			imported.GroupBindings = std::vector<GroupSubtypeBinding>{};
			const auto archiveBytes = ImageGraphHistoryArchiveBytes(imported.Source);
			const auto projectionBytes = DocumentRetainedPayloadBytes(imported.Graph);
			const auto diagnosticBytes = ImageGraphHistoryDiagnosticBytes(imported.Diagnostics);
			const auto preview = imported.ReferencePreview();
			uint64_t held = (preview ? preview->Rgba.size() : 0) + diagnosticBytes.value_or(0);
			if (!archiveBytes || !projectionBytes || !diagnosticBytes || *archiveBytes > available ||
				*projectionBytes > available - *archiveBytes ||
				held >= available - *archiveBytes - *projectionBytes)
				return fail(Status::LimitExceeded, "Source history candidates exceed live bytes");
			Image thumbnail;
			if (preview) {
				thumbnail = {
					preview->Width,
					preview->Height,
					std::vector<uint8_t>(preview->Rgba.begin(), preview->Rgba.end()),
					preview->Hash
				};
				const auto extra = thumbnail.Pixels.capacity() - preview->Rgba.size();
				if (extra >= available - *archiveBytes - *projectionBytes - held)
					return fail(
						Status::LimitExceeded, "Source history thumbnail capacity exceeds live bytes"
					);
				held += extra;
			}
			remaining -= *archiveBytes + *projectionBytes + held;
			Archive = std::move(imported.Source);
			Projection = std::move(imported.Graph);
			Diagnostics = std::move(imported.Diagnostics);
			Thumbnail = std::move(thumbnail);
			Changed = true;
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "Source history allocation refused"
			};
			return false;
		}
	};
}
