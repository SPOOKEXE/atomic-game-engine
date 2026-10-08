#include "ImageCacheAnnotation.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraphio/SourceImageEdit.hpp>

#include <algorithm>
#include <limits>
#include <new>

namespace engine::imagegraphio {
	using namespace imagegraph;
	Status ApplySourceImageEdit(
		const Document &document,
		const SourceImageFrameObservation &prepared,
		const GroupReplayState &replay,
		const SourceImageEditOptions &options,
		Document &result,
		GroupReplayState &resultReplay,
		bool &changed,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE_CAT("source image authoring", core::ProfileCategory::Script);
		const auto fail = [&](Status status, const char *message) {
			diagnostic = {status, prepared.Controls.Authored.Id, {}, message};
			return status;
		};
		if (options.Action != SourceImageAction::MatchLength && options.Action != SourceImageAction::Cache &&
			options.Action != SourceImageAction::RemoveCache)
			return fail(Status::InvalidValue, "unknown source image action");
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		const auto resultBytes =
			&result == &document ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		const auto controlBytes = HostCaptureRetainedPayloadBytes(prepared.Controls);
		uint64_t used = 0;
		const auto spend = [&](uint64_t bytes) {
			if (bytes > maximumBytes - used) return false;
			used += bytes;
			return true;
		};
		if (!documentBytes || !resultBytes || !controlBytes || !spend(*documentBytes) ||
			!spend(*resultBytes) || !spend(*controlBytes) ||
			!spend(sizeof(prepared) - sizeof(prepared.Controls)) ||
			!spend(prepared.Frames.capacity() * sizeof(Image)) ||
			(prepared.EncodedCache && !spend(prepared.EncodedCache->capacity() + 1)) ||
			(&resultReplay != &replay && !spend(resultReplay.RetainedBytes())))
			return fail(
				Status::LimitExceeded, "source image transaction retained owners exceed operation bounds"
			);
		for (const auto &image : prepared.Frames)
			if (!spend(image.Pixels.capacity()))
				return fail(Status::LimitExceeded, "prepared image storage exceeds operation bounds");
		const uint64_t external = used;
		if (!spend(replay.RetainedBytes()))
			return fail(Status::LimitExceeded, "prepared replay exceeds operation bounds");
		const auto selected = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
			return n.Id == prepared.Controls.Authored.Id;
		});
		if (selected == document.Nodes.end() || *selected != prepared.Controls.Authored ||
			!detail::SourceImageCacheType(selected->Type) ||
			!ValidFrameTime(
				{prepared.Controls.Tick, prepared.Controls.Subframe, prepared.Controls.NegativeFrame}
			) ||
			!prepared.Controls.Outputs.empty() || !prepared.Controls.Images.empty() ||
			!prepared.Controls.ImageArrays.empty() || !replay.InstancesBound() ||
			replay.AuthoringRevision() != options.AuthoringRevision ||
			prepared.AuthoringRevision != options.AuthoringRevision ||
			prepared.InputRevision != options.InputRevision || !options.AuthoringRevision ||
			!options.NextAuthoringRevision || options.NextAuthoringRevision == options.AuthoringRevision)
			return fail(
				Status::InvalidValue,
				"source image action requires current prepared controls and bound replay"
			);
		if (prepared.Kind != SourceImageFrameKind::Live && prepared.Kind != SourceImageFrameKind::Cached &&
			prepared.Kind != SourceImageFrameKind::ControlsOnly)
			return fail(Status::InvalidValue, "unknown source image frame observation kind");
		if (options.Action != SourceImageAction::RemoveCache && prepared.Kind != SourceImageFrameKind::Live)
			return fail(
				Status::InvalidValue, "source image authoring requires live sprites rather than saved cache"
			);
		if (prepared.Frames.size() > bake::SpriteCacheLimits::MaximumFrames ||
			(selected->Type == "pc.image" && prepared.Frames.size() > 1))
			return fail(Status::LimitExceeded, "prepared source sprite count exceeds native bounds");
		uint64_t pixels = 0;
		for (const auto &image : prepared.Frames) {
			if (image.Format != SurfaceFormat::RGBA8Unorm ||
				!ValidSurfaceLayout(image, bake::SpriteCacheLimits::MaximumDimension, maximumBytes))
				return fail(Status::InvalidValue, "prepared source sprites require exact RGBA8 byte layouts");
			const uint64_t count = uint64_t(image.Width) * image.Height;
			if (count > bake::SpriteCacheLimits::MaximumPixels - pixels)
				return fail(Status::LimitExceeded, "prepared source sprite pixels exceed native bounds");
			pixels += count;
		}
		if (selected->SourceProperties.size() > Limits::MaximumPropertiesPerNode)
			return fail(Status::LimitExceeded, "source image properties exceed native count bounds");
		for (const auto name :
			 {detail::ImageCacheUse,
			  detail::ImageCacheData,
			  detail::ImageCacheLayout,
			  detail::ImageCacheHash})
			if (std::count_if(
					selected->SourceProperties.begin(),
					selected->SourceProperties.end(),
					[&](const auto &value) { return value.Port == name; }
				) > 1)
				return fail(Status::DuplicateId, "source image cache properties must be unique");
		const auto property = [&](std::string_view name) -> const Value * {
			const auto found = std::find_if(
				selected->SourceProperties.begin(), selected->SourceProperties.end(), [&](const auto &value) {
					return value.Port == name;
				}
			);
			return found == selected->SourceProperties.end() ? nullptr : &found->Data;
		};
		const auto noChange = [&]() {
			changed = false;
			diagnostic = {};
			return Status::Ok;
		};
		std::optional<std::array<char, 64>> hash;
		std::string_view layout;
		if (options.Action == SourceImageAction::MatchLength) {
			if (selected->Type != "pc.image_animated" || !document.Timeline ||
				!document.Timeline->SourceBounds)
				return fail(
					Status::UnsupportedExecution,
					"Match Length requires Image Animated and retained source timeline intent"
				);
			if (prepared.Frames.empty() || document.Timeline->Frames == prepared.Frames.size())
				return noChange();
		} else if (options.Action == SourceImageAction::RemoveCache) {
			const auto *value = property(detail::ImageCacheUse);
			if (!value) return noChange();
			const auto *enabled = std::get_if<bool>(value);
			if (!enabled) return fail(Status::InvalidValue, "source image cache_use must be boolean");
			if (!*enabled) return noChange();
		} else {
			if (!prepared.EncodedCache || !prepared.CacheLayout ||
				(selected->Type == "pc.image" && prepared.Frames.size() != 1))
				return fail(
					Status::UnsupportedExecution,
					"Cache requires a verified live sprite encoding and explicit byte layout"
				);
			hash = bake::SpriteCacheDataHash(*prepared.EncodedCache);
			layout = bake::SpriteCacheLayoutName(*prepared.CacheLayout);
			if (!hash || layout.empty())
				return fail(Status::InvalidValue, "prepared source cache text or layout is outside bounds");
			const auto equalsText = [&](std::string_view port, std::string_view value) {
				const auto *old = property(port);
				const auto *text = old ? std::get_if<std::string>(old) : nullptr;
				return text && *text == value;
			};
			const auto *use = property(detail::ImageCacheUse);
			const auto *enabled = use ? std::get_if<bool>(use) : nullptr;
			if (enabled && *enabled && equalsText(detail::ImageCacheData, *prepared.EncodedCache) &&
				equalsText(detail::ImageCacheLayout, layout) &&
				equalsText(detail::ImageCacheHash, std::string_view(hash->data(), hash->size())))
				return noChange();
		}
		// The old candidate table remains resident during reserve. Charge the complete
		// replacement backing and all new text before cloning either authoring owner.
		uint64_t extra = 0;
		if (options.Action == SourceImageAction::Cache) {
			const auto textClone = [](std::string_view text) {
				return std::max(text.size(), std::string{}.capacity()) + 1;
			};
			extra = (selected->SourceProperties.size() + 4) * sizeof(AuthoredValue) +
					prepared.EncodedCache->capacity() + 1 + textClone(layout) + 65 +
					textClone(detail::ImageCacheUse) + textClone(detail::ImageCacheData) +
					textClone(detail::ImageCacheLayout) + textClone(detail::ImageCacheHash);
		}
		if (!spend(*documentBytes) || !spend(extra))
			return fail(Status::LimitExceeded, "source image candidate exceeds operation bounds");
		const uint64_t candidateAllowance = *documentBytes + extra;
		Document candidate = document;
		auto &node = candidate.Nodes[size_t(selected - document.Nodes.begin())];
		const auto set = [&](std::string_view port, Value value) {
			auto old =
				std::find_if(node.SourceProperties.begin(), node.SourceProperties.end(), [&](const auto &v) {
					return v.Port == port;
				});
			if (old == node.SourceProperties.end())
				node.SourceProperties.push_back({std::string(port), std::move(value)});
			else
				old->Data = std::move(value);
		};
		if (options.Action == SourceImageAction::MatchLength) {
			candidate.Timeline->Frames = prepared.Frames.size();
			if (ProjectSourceTimelineWindow(*candidate.Timeline, diagnostic) != Status::Ok)
				return diagnostic.Code;
		} else if (options.Action == SourceImageAction::RemoveCache)
			set(detail::ImageCacheUse, false);
		else {
			size_t missing = 0;
			for (const auto name :
				 {detail::ImageCacheUse,
				  detail::ImageCacheData,
				  detail::ImageCacheLayout,
				  detail::ImageCacheHash})
				if (!property(name)) ++missing;
			if (missing > Limits::MaximumPropertiesPerNode - node.SourceProperties.size())
				return fail(
					Status::LimitExceeded, "source image cache properties exceed native count bounds"
				);
			node.SourceProperties.reserve(node.SourceProperties.size() + missing);
			if (node.SourceProperties.capacity() > selected->SourceProperties.size() + 4)
				return fail(Status::LimitExceeded, "source image property reserve exceeds admitted backing");

			set(detail::ImageCacheUse, true);
			set(detail::ImageCacheData, *prepared.EncodedCache);
			set(detail::ImageCacheLayout, std::string(layout));
			set(detail::ImageCacheHash, std::string(hash->data(), hash->size()));
			candidate.FormatVersion = std::max(candidate.FormatVersion, 9u);
		}
		const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
		if (!candidateBytes || *candidateBytes > candidateAllowance)
			return fail(Status::LimitExceeded, "source image candidate retained storage exceeds admission");
		GroupReplayState nextReplay;
		if (RebindProjectedGroupReplay(
				candidate,
				replay,
				options.NextAuthoringRevision,
				nextReplay,
				diagnostic,
				maximumBytes - external
			) != Status::Ok)
			return diagnostic.Code;
		if (nextReplay.RetainedBytes() > maximumBytes - external - replay.RetainedBytes() - *candidateBytes)
			return fail(
				Status::LimitExceeded, "source image candidate and rebound replay exceed operation bounds"
			);
		core::Metrics::Count("imagegraphio.source_image_edit_bytes", *candidateBytes);
		core::Metrics::Count("imagegraphio.source_image_edit_frames", prepared.Frames.size());
		result = std::move(candidate);
		resultReplay = std::move(nextReplay);
		changed = true;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			Status::LimitExceeded,
			prepared.Controls.Authored.Id,
			{},
			"source image authoring allocation failed"
		};
		return diagnostic.Code;
	}
}
