#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/PxcxThumbnail.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		using engine::bake::PxcxArchive;
		using engine::bake::PxcxLimits;
		std::optional<uint64_t> ArchiveBytes(const PxcxArchive &archive) {
			uint64_t bytes = sizeof(archive);
			const auto add = [&](uint64_t count) {
				if (count > UINT64_MAX - bytes) return false;
				bytes += count;
				return true;
			};
			if (archive.OriginalBytes.size() > PxcxLimits::MaximumArchiveBytes ||
				archive.GraphJson.size() > PxcxLimits::MaximumGraphJsonBytes ||
				archive.MetadataPayload.size() > PxcxLimits::MaximumMetadataBytes ||
				archive.Nodes.size() > PxcxLimits::MaximumNodes ||
				archive.Links.size() > PxcxLimits::MaximumLinks ||
				archive.Nodes.capacity() > UINT64_MAX / sizeof(engine::bake::PxcxNodeFact) ||
				archive.Links.capacity() > UINT64_MAX / sizeof(engine::bake::PxcxLinkFact) ||
				!add(archive.OriginalBytes.capacity()) || !add(archive.ThumbnailRgba.capacity()) ||
				!add(archive.MetadataPayload.capacity()) || !add(archive.MetadataText.capacity() + 1) ||
				!add(archive.GraphJson.capacity() + 1) ||
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
	bool WritePxcxPreparedThumbnail(
		const engine::bake::PxcxArchive &checkedCandidate,
		const engine::imagegraph::Image &preparedPreview,
		std::vector<std::byte> &out,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maxBytes
	) try {
		ENGINE_PROFILE("image composer pxc thumbnail");
		diagnostic = {};
		const auto fail = [&](Status status, std::string message) {
			diagnostic = {status, {}, {}, std::move(message)};
			return false;
		};
		if (maxBytes == 0 || maxBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "PXC thumbnail operation budget is outside native bounds");
		const auto archiveBytes = ArchiveBytes(checkedCandidate);
		if (!archiveBytes || checkedCandidate.OriginalBytes.empty())
			return fail(Status::InvalidValue, "PXC thumbnail needs a checked serialized candidate");
		if (!ValidSurfaceLayout(preparedPreview, Limits::MaximumDimension, Limits::MaximumOutputBytes))
			return fail(Status::InvalidValue, "PXC thumbnail preview layout is invalid");
		uint64_t remaining = maxBytes;
		const auto admit = [&](uint64_t amount) {
			if (amount > remaining) return false;
			remaining -= amount;
			return true;
		};
		// Input plus candidate/readback archive payloads coexist during the checked codec write.
		// The three encoded slots admit compressor output, staged bytes and validation readback.
		constexpr uint64_t encodedSlot = uint64_t(PxcxLimits::MaximumGraphCompressedBytes) +
										 PxcxLimits::MaximumThumbnailCompressedBytes +
										 PxcxLimits::MaximumMetadataBytes + 24;
		for (unsigned copy = 0; copy < 4; ++copy)
			if (!admit(*archiveBytes))
				return fail(Status::LimitExceeded, "PXC thumbnail archive clones exceed budget");
		if (!admit(preparedPreview.Pixels.capacity()) || !admit(sizeof(Image) * 2) ||
			!admit(out.capacity()) || !admit(sizeof(out) * 4) || !admit(PxcxLimits::ThumbnailRgbaBytes) ||
			!admit(encodedSlot * 3))
			return fail(
				Status::LimitExceeded, "PXC thumbnail preview, prior output and codec staging exceed budget"
			);
		if (!FiniteSurfaceSamples(preparedPreview))
			return fail(Status::InvalidValue, "PXC thumbnail preview contains nonfinite samples");
		PxcxArchive candidate = checkedCandidate;
		const auto actualBytes = ArchiveBytes(candidate);
		if (!actualBytes || *actualBytes > *archiveBytes)
			return fail(Status::LimitExceeded, "PXC thumbnail actual clone backing exceeds admission");
		candidate.HasThumbnailBlock = true;
		candidate.ThumbnailRgba.resize(PxcxLimits::ThumbnailRgbaBytes);
		if (candidate.ThumbnailRgba.capacity() >
			PxcxLimits::ThumbnailRgbaBytes + checkedCandidate.ThumbnailRgba.capacity())
			return fail(Status::LimitExceeded, "PXC thumbnail actual pixel backing exceeds admission");
		Image target{256, 256, {}, 0, SurfaceFormat::RGBA8Unorm};
		target.Pixels.swap(candidate.ThumbnailRgba);
		const uint64_t shorter = std::min(preparedPreview.Width, preparedPreview.Height);
		for (uint32_t y = 0; y < 256; ++y)
			for (uint32_t x = 0; x < 256; ++x) {
				// Exact cover geometry at destination pixel centres, then native nearest source texels.
				const auto sx = uint32_t(
					((uint64_t(preparedPreview.Width) - shorter) * 256 + (2 * x + 1) * shorter) / 512
				);
				const auto sy = uint32_t(
					((uint64_t(preparedPreview.Height) - shorter) * 256 + (2 * y + 1) * shorter) / 512
				);
				SurfacePixel pixel{};
				if (!LoadSurfacePixel(preparedPreview, sx, sy, pixel) ||
					!StoreSurfacePixel(target, x, y, pixel))
					return fail(Status::InvalidValue, "PXC thumbnail preview sample conversion failed");
			}
		candidate.ThumbnailRgba.swap(target.Pixels);
		std::vector<std::byte> written;
		std::string failure;
		if (!engine::bake::WritePxcx(candidate, written, failure))
			return fail(Status::InvalidValue, std::move(failure));
		if (written.capacity() > encodedSlot)
			return fail(Status::LimitExceeded, "PXC thumbnail actual encoded backing exceeds admission");
		engine::core::Metrics::Count("imagegraph.pxc_thumbnail.pixels", 256 * 256);
		engine::core::Metrics::Count("imagegraph.pxc_thumbnail.encoded_bytes", double(written.size()));
		engine::core::Metrics::Count(
			"imagegraph.pxc_thumbnail.payload_reservation_bytes", double(maxBytes - remaining)
		);
		out = std::move(written);
		return true;
	} catch (const std::bad_alloc &) {
		diagnostic = {engine::imagegraph::Status::LimitExceeded, {}, {}, "PXC thumbnail allocation failed"};
		return false;
	}
}
