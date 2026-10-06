#include "ImageGraphFilePublish.hpp"
#include "ImageGraphFileSetPublish.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/PxcxThumbnail.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>

#include <algorithm>
#include <new>
#include <studio/PxcxSave.hpp>

namespace studio {
	namespace {
		using namespace engine::imagegraph;
		bool Reject(Diagnostic &diagnostic, std::string reason, Status code = Status::InvalidValue) {
			diagnostic = {code, {}, "path", std::move(reason)};
			return false;
		}
		std::optional<uint64_t> RetainedArchiveBytes(const engine::bake::PxcxArchive &archive) {
			uint64_t bytes = sizeof(archive);
			const auto add = [&](uint64_t amount) {
				if (amount > UINT64_MAX - bytes) return false;
				bytes += amount;
				return true;
			};
			if (!add(archive.OriginalBytes.capacity()) || !add(archive.ThumbnailRgba.capacity()) ||
				!add(archive.MetadataPayload.capacity()) || !add(archive.MetadataText.capacity() + 1) ||
				!add(archive.GraphJson.capacity() + 1) ||
				archive.Nodes.capacity() > UINT64_MAX / sizeof(engine::bake::PxcxNodeFact) ||
				archive.Links.capacity() > UINT64_MAX / sizeof(engine::bake::PxcxLinkFact) ||
				!add(archive.Nodes.capacity() * sizeof(engine::bake::PxcxNodeFact)) ||
				!add(archive.Links.capacity() * sizeof(engine::bake::PxcxLinkFact)))
				return std::nullopt;
			for (const auto &node : archive.Nodes)
				if (!add(node.Id.capacity() + 1) || !add(node.Type.capacity() + 1)) return std::nullopt;
			for (const auto &link : archive.Links)
				if (!add(link.FromNode.capacity() + 1) || !add(link.ToNode.capacity() + 1))
					return std::nullopt;
			return bytes;
		}

	}

	bool SavePxcxCollection(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		std::string_view collectionId,
		std::optional<std::string_view> managerJson,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("studio pxc collection save");
		diagnostic = {};
		if (destination.empty() || destination.native().size() > 4096 || destination.extension() != ".pxcc")
			return Reject(diagnostic, "enter a .pxcc collection path");
		engine::imagegraphio::PxcxCollectionSave prepared;
		if (!engine::imagegraphio::PreparePxcxCollectionSave(
				source, collectionId, managerJson, prepared, diagnostic, maximumBytes
			))
			return false;
		std::array<detail::ImageGraphFilePublication, 2> files;
		files[0] = {
			destination, std::as_bytes(std::span(prepared.GraphJson.data(), prepared.GraphJson.size()))
		};
		size_t count = 1;
		if (prepared.MetadataJson) {
			auto metadataPath = destination;
			metadataPath.replace_extension(".meta");
			const auto &metadata = *prepared.MetadataJson;
			files[1] = {std::move(metadataPath), std::as_bytes(std::span(metadata.data(), metadata.size()))};
			count = 2;
		}
		return detail::PublishImageGraphFileSet(std::span(files.data(), count), diagnostic, maximumBytes);
	} catch (const std::bad_alloc &) {
		return Reject(
			diagnostic, "collection save allocation failed", engine::imagegraph::Status::LimitExceeded
		);
	} catch (const std::filesystem::filesystem_error &) {
		return Reject(diagnostic, "collection save path is invalid");
	}

	bool SavePxcxProjection(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		const engine::imagegraph::Document &authored,
		engine::imagegraph::FrameTime captureTime,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		diagnostic = {};
		const auto reject = [&](std::string reason) {
			diagnostic = {Status::InvalidValue, {}, "path", std::move(reason)};
			return false;
		};
		if (destination.empty()) return reject("enter a PXC archive path");
		engine::imagegraphio::PxcxImport imported;
		std::string failure;
		if (!engine::imagegraphio::ImportPxcxImageGraph(source, imported, failure)) return reject(failure);
		std::vector<std::byte> bytes;
		if (!engine::imagegraphio::WritePxcxProjection(imported, authored, captureTime, bytes, diagnostic))
			return false;
		return detail::PublishImageGraphFile(
			destination, bytes, diagnostic, engine::bake::PxcxLimits::MaximumArchiveBytes
		);
	}

	bool SavePxcxProjectionAndAdopt(
		const std::filesystem::path &destination,
		const engine::bake::PxcxArchive &source,
		const engine::imagegraph::Document &authored,
		engine::imagegraph::FrameTime captureTime,
		PxcxPublishedSave &published,
		const PxcxPreparedSavePreview *preview,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maxBytes
	) try {
		ENGINE_PROFILE("studio pxc prepared save");
		using namespace engine::imagegraph;
		diagnostic = {};
		if (destination.empty()) return Reject(diagnostic, "enter a PXC archive path");
		if (!maxBytes || maxBytes > Limits::MaximumEvaluationBytes)
			return Reject(diagnostic, "PXC save budget is outside native bounds", Status::LimitExceeded);
		if (preview) {
			const auto &current = preview->Current;
			if (!current.DocumentRevision || !current.InputRevision || !ValidFrameTime(current.Frame) ||
				current.Frame != captureTime || current != preview->Completed ||
				std::find(authored.Outputs.begin(), authored.Outputs.end(), current.Binding) ==
					authored.Outputs.end())
				return Reject(diagnostic, "refresh the selected image preview before saving its thumbnail");
		}
		const auto sourceBytes = RetainedArchiveBytes(source);
		const auto priorBytes = RetainedArchiveBytes(published.Archive);
		uint64_t remaining = maxBytes;
		const auto admit = [&](uint64_t amount) {
			if (amount > remaining) return false;
			remaining -= amount;
			return true;
		};
		if (!sourceBytes || !priorBytes || !admit(*sourceBytes) || !admit(*priorBytes) ||
			!admit(published.ReferencePreview.Pixels.capacity()) || !admit(sizeof(PxcxPublishedSave)))
			return Reject(diagnostic, "retained PXC save state exceeds budget", Status::LimitExceeded);
		const auto identityBytes = [&](const PxcxPreviewIdentity &identity) {
			uint64_t bytes = 0;
			for (const std::string *text :
				 {&identity.Binding.Id, &identity.Binding.NodeId, &identity.Binding.Port}) {
				if (text->size() > Limits::MaximumTextBytes || text->capacity() >= UINT64_MAX - bytes)
					return std::optional<uint64_t>{};
				bytes += text->capacity() + 1;
			}
			return std::optional(bytes);
		};
		if (published.ThumbnailIdentity) {
			const auto held = identityBytes(*published.ThumbnailIdentity);
			if (!held || !admit(*held))
				return Reject(diagnostic, "PXC preview identity exceeds budget", Status::LimitExceeded);
		}
		const PxcxPreviewIdentity *nextIdentity =
			preview ? &preview->Completed
					: (published.ThumbnailIdentity ? &*published.ThumbnailIdentity : nullptr);
		if (nextIdentity) {
			const auto held = identityBytes(*nextIdentity);
			if (!held || !admit(*held))
				return Reject(diagnostic, "PXC preview identity clone exceeds budget", Status::LimitExceeded);
		}
		const uint64_t previewBytes = preview ? preview->Pixels.Pixels.capacity() : 0;
		if (previewBytes > remaining)
			return Reject(diagnostic, "borrowed PXC preview exceeds budget", Status::LimitExceeded);
		const uint64_t phaseBytes = remaining - previewBytes;
		std::vector<std::byte> bytes;
		{
			if (*sourceBytes > phaseBytes / 2)
				return Reject(
					diagnostic, "PXC projection archive copies exceed budget", Status::LimitExceeded
				);
			engine::imagegraphio::PxcxImport imported;
			std::string failure;
			if (!engine::imagegraphio::ImportPxcxImageGraph(source, imported, failure))
				return Reject(diagnostic, std::move(failure));
			imported.Options.MaximumOperationBytes = phaseBytes;
			if (!engine::imagegraphio::WritePxcxProjection(
					imported, authored, captureTime, bytes, diagnostic
				))
				return false;
		}
		if (bytes.capacity() > phaseBytes)
			return Reject(diagnostic, "serialized PXC candidate exceeds budget", Status::LimitExceeded);
		std::string failure;
		engine::bake::PxcxArchive candidate;
		if (!engine::bake::ReadPxcx(bytes, candidate, failure)) return Reject(diagnostic, std::move(failure));
		std::vector<std::byte>().swap(bytes);
		if (preview) {
			if (!engine::imagegraphexport::WritePxcxPreparedThumbnail(
					candidate, preview->Pixels, bytes, diagnostic, remaining
				))
				return false;
		} else if (!published.Archive.OriginalBytes.empty()) {
			// Presentation comes from the last publication only after source projection validation.
			// Keep the original authoring archive and record provenance unchanged for undo.
			if (published.Archive.ThumbnailRgba.capacity() > remaining)
				return Reject(diagnostic, "published thumbnail copy exceeds budget", Status::LimitExceeded);
			candidate.HasThumbnailBlock = published.Archive.HasThumbnailBlock;
			candidate.ThumbnailRgba = published.Archive.ThumbnailRgba;
			if (!engine::bake::WritePxcx(candidate, bytes, failure))
				return Reject(diagnostic, std::move(failure));
		} else {
			bytes.swap(candidate.OriginalBytes);
		}
		// Readback and the display reference must be ready before publication, so adoption cannot
		// fail after replacing the file. Vendor decoder workspace has its existing bake caps.
		const auto candidateBytes = RetainedArchiveBytes(candidate);
		if (!candidateBytes || *candidateBytes > phaseBytes / 2 ||
			bytes.capacity() > (phaseBytes - *candidateBytes * 2) / 2 ||
			engine::bake::PxcxLimits::ThumbnailRgbaBytes * 3 >
				phaseBytes - *candidateBytes * 2 - bytes.capacity() * 2)
			return Reject(diagnostic, "PXC publication readback exceeds budget", Status::LimitExceeded);
		PxcxPublishedSave next;
		if (!engine::bake::ReadPxcx(bytes, next.Archive, failure))
			return Reject(diagnostic, std::move(failure));
		if (!next.Archive.ThumbnailRgba.empty()) {
			next.ReferencePreview = Image{256, 256, next.Archive.ThumbnailRgba, 0, SurfaceFormat::RGBA8Unorm};
			next.ReferencePreview.Hash = SurfaceHash(next.ReferencePreview);
		}
		if (nextIdentity) {
			next.ThumbnailIdentity = *nextIdentity;
			const auto requested = identityBytes(*nextIdentity);
			const auto actual = identityBytes(*next.ThumbnailIdentity);
			if (!requested || !actual || *actual > *requested)
				return Reject(
					diagnostic, "PXC preview identity backing exceeds admission", Status::LimitExceeded
				);
		}
		if (!detail::PublishImageGraphFile(
				destination, bytes, diagnostic, engine::bake::PxcxLimits::MaximumArchiveBytes
			))
			return false;
		engine::core::Metrics::Count("studio.pxc_save.published_bytes", double(bytes.size()));
		published = std::move(next);
		return true;
	} catch (const std::bad_alloc &) {
		return Reject(diagnostic, "PXC save allocation failed", engine::imagegraph::Status::LimitExceeded);
	}
}
