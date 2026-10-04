#include "GraphImageCacheHost.hpp"

#include "GraphRasterHost.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraphexport {
	using namespace imagegraph;
	bool ReadGraphSavedImageCache(
		const HostNodeInvocation &in,
		std::span<const GraphImageCacheLayoutObservation> observations,
		std::vector<Image> &frames,
		bool &enabled,
		std::string &failure
	) try {
		ENGINE_PROFILE("source saved image cache");
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		if (in.Authored.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
			observations.size() > 64)
			return fail("source image cache metadata exceeds native bounds");
		const Value *use = nullptr, *data = nullptr, *layout = nullptr, *hash = nullptr;
		for (const auto &property : in.Authored.SourceProperties) {
			const Value **target = property.Port == "cache_use"							? &use
								   : property.Port == "cache_data"						? &data
								   : property.Port == "composer_sprite_cache_layout"	? &layout
								   : property.Port == "composer_sprite_cache_data_hash" ? &hash
																						: nullptr;
			if (!target) continue;
			if (*target) return fail("source image cache properties must be unique");
			*target = &property.Data;
		}
		const auto *active = use ? std::get_if<bool>(use) : nullptr;
		if (use && !active) return fail("source image cache_use must be boolean");
		if (!active || !*active) {
			enabled = false;
			return true;
		}
		const auto *text = data ? std::get_if<std::string>(data) : nullptr;
		if (!text) return fail("enabled source image cache requires saved cache text");
		const auto actual = bake::SpriteCacheDataHash(*text);
		if (!actual) return fail("source image cache text exceeds native representation bounds");
		std::optional<bake::SpriteCacheLayout> observed;
		if (bool(layout) != bool(hash))
			return fail("native image cache layout and data hash must be supplied together");
		if (layout) {
			const auto *name = std::get_if<std::string>(layout);
			const auto *identity = std::get_if<std::string>(hash);
			if (!name || !identity || *identity != std::string_view(actual->data(), actual->size()))
				return fail("native image cache annotation does not match exact saved text");
			observed = bake::ParseSpriteCacheLayoutName(*name);
			if (!observed) return fail("native image cache annotation names an unknown byte layout");
		}
		bool haveForeign = false;
		for (const auto &observation : observations) {
			if (observation.NodeId.empty() || observation.NodeId.size() > Limits::MaximumTextBytes ||
				observation.DataHash.size() != 64 ||
				std::any_of(
					observation.DataHash.begin(),
					observation.DataHash.end(),
					[](char byte) { return !(byte >= '0' && byte <= '9') && !(byte >= 'a' && byte <= 'f'); }
				) ||
				bake::SpriteCacheLayoutName(observation.Layout).empty())
				return fail("image cache layout observation is malformed");
			if (observation.NodeId != in.Authored.Id) continue;
			if (haveForeign) return fail("image cache layout observation is duplicated");
			haveForeign = true;
			if (observation.DataHash != std::string_view(actual->data(), actual->size()))
				return fail("image cache layout observation is stale");
			if (observed && *observed != observation.Layout)
				return fail("foreign and native cache layouts conflict");
			observed = observation.Layout;
		}
		if (!observed)
			return fail("foreign image cache requires an explicit exact-data byte layout observation");
		if (frames.capacity() > in.MaximumOperationBytes / sizeof(Image))
			return fail("prior cache frame table exceeds operation bounds");
		uint64_t prior = frames.capacity() * sizeof(Image);
		for (const auto &frame : frames) {
			if (frame.Pixels.capacity() > in.MaximumOperationBytes - prior)
				return fail("prior saved image cache storage exceeds operation bounds");
			prior += frame.Pixels.capacity();
		}
		std::vector<bake::SpriteCacheFrame> decoded;
		if (!bake::ReadSpriteCache(
				*text,
				*observed,
				decoded,
				failure,
				(in.MaximumOperationBytes - prior) / 2,
				in.Authored.Type == "pc.image" ? bake::SpriteCacheShape::Sprite
											   : bake::SpriteCacheShape::Array
			))
			return false;
		uint64_t decodedBytes = decoded.capacity() * sizeof(bake::SpriteCacheFrame);
		for (const auto &frame : decoded)
			decodedBytes += frame.Rgba.capacity();
		const uint64_t table = decoded.size() * sizeof(Image);
		if (prior > in.MaximumOperationBytes || decodedBytes > in.MaximumOperationBytes - prior ||
			table > in.MaximumOperationBytes - prior - decodedBytes)
			return fail("saved image cache conversion overlaps exceed operation bounds");
		std::vector<Image> candidate;
		candidate.reserve(decoded.size());
		if (candidate.capacity() > decoded.size())
			return fail("saved image cache reserve exceeds admitted table");
		for (auto &frame : decoded) {
			Image image;
			image.Width = frame.Width;
			image.Height = frame.Height;
			image.Pixels = std::move(frame.Rgba);
			image.Hash = SurfaceHash(image);
			candidate.push_back(std::move(image));
		}
		core::Metrics::Count("imagegraphexport.source_cache_frames", candidate.size());
		frames = std::move(candidate);
		enabled = true;
		return true;
	} catch (const std::bad_alloc &) {
		failure = "saved image cache allocation failed";
		return false;
	}

	bool PrepareGraphSourceImages(
		const HostNodeCapture &controls,
		std::span<const GraphFileGrant> grants,
		const assets::ContentPolicy &policy,
		uint64_t authoringRevision,
		uint64_t inputRevision,
		bool encodeCache,
		imagegraphio::SourceImageFrameObservation &result,
		std::string &failure,
		uint64_t maximumBytes
	) try {
		EvaluationRequest request;
		request.Tick = controls.Tick;
		request.Subframe = controls.Subframe;
		request.NegativeFrame = controls.NegativeFrame;
		HostNodeInvocation in{controls.Authored, request, controls.Inputs, {}, maximumBytes};
		ENGINE_PROFILE("source live image cache admission");
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		const auto controlBytes = HostCaptureRetainedPayloadBytes(controls);
		if (!controlBytes || *controlBytes > maximumBytes / 4 || !controls.Outputs.empty() ||
			!controls.Images.empty() || !controls.ImageArrays.empty())
			return fail("source image admission requires bounded prepared controls without runtime outputs");
		const bool scalar = in.Authored.Type == "pc.image";
		if (!scalar && in.Authored.Type != "pc.image_sequence" && in.Authored.Type != "pc.image_animated")
			return fail("source image admission requires an image, sequence or animated node");
		const auto input = std::find_if(in.Inputs.begin(), in.Inputs.end(), [&](const auto &value) {
			return value.Port == (in.Authored.Type == "pc.image_sequence" ? "paths" : "path");
		});
		if (input == in.Inputs.end()) return fail("source image admission requires resolved path controls");
		const auto priorControls = HostCaptureRetainedPayloadBytes(result.Controls);
		uint64_t prior = sizeof(result) + result.Frames.capacity() * sizeof(Image);
		if (!priorControls || *priorControls > in.MaximumOperationBytes / 4 ||
			prior > in.MaximumOperationBytes / 4 - *priorControls)
			return fail("prior source image observation exceeds admission budget");
		prior += *priorControls;
		for (const auto &image : result.Frames) {
			if (image.Pixels.capacity() > in.MaximumOperationBytes / 4 - prior)
				return fail("prior source image observation exceeds admission budget");
			prior += image.Pixels.capacity();
		}
		if (result.EncodedCache) {
			if (result.EncodedCache->capacity() + 1 > in.MaximumOperationBytes / 4 - prior)
				return fail("prior source cache text exceeds admission budget");
			prior += result.EncodedCache->capacity() + 1;
		}
		const auto pathBytes = ValueClonePayloadBytes(input->Data);
		if (!pathBytes || *pathBytes > maximumBytes / 4 || *controlBytes > maximumBytes - prior ||
			*controlBytes > maximumBytes - prior - *controlBytes ||
			*pathBytes > maximumBytes - prior - *controlBytes * 2)
			return fail("source image admission control and path clones exceed byte budget");
		const uint64_t temporary =
			2048 + controls.Authored.Id.size() + 1 + bake::SpriteCacheLimits::MaximumFrames * sizeof(Image);
		if (temporary > maximumBytes - prior - *controlBytes * 2 - *pathBytes)
			return fail("source image temporary tables exceed admission bounds");
		const uint64_t external = prior + *controlBytes + temporary;
		const uint64_t admission = maximumBytes - external - *controlBytes - *pathBytes;
		Node raw;
		raw.Id = in.Authored.Id;
		raw.Type = scalar ? "pc.image" : "pc.image_sequence";
		std::vector<AuthoredValue> inputs{{scalar ? "path" : "paths", input->Data}, {"padding", Vector4{}}};
		if (!scalar) {
			inputs.push_back({"canvas_size", EnumValue{0}});
			inputs.push_back({"sizing_method", EnumValue{0}});
		}
		HostNodeCapture decoded;
		const uint64_t available = admission;
		if (!CaptureGraphRaster(
				{raw, in.Request, inputs, {}, available / 3, in.Timeline, SurfaceFormat::RGBA8Unorm},
				grants,
				policy,
				decoded,
				failure
			))
			return false;
		imagegraphio::SourceImageFrameObservation candidate;
		candidate.Controls = controls;
		candidate.AuthoringRevision = authoringRevision;
		candidate.InputRevision = inputRevision;
		if (scalar) {
			if (decoded.Images.size() != 1)
				return fail("source still admission produced an unexpected frame ledger");
			candidate.Frames.push_back(std::move(decoded.Images[0].Data));
		} else {
			if (decoded.ImageArrays.size() != 1)
				return fail("source sequence admission produced an unexpected frame ledger");
			candidate.Frames = std::move(decoded.ImageArrays[0].Frames);
		}
		decoded = {};
		const auto candidateControls = HostCaptureRetainedPayloadBytes(candidate.Controls);
		uint64_t retained = sizeof(candidate) + candidate.Frames.capacity() * sizeof(Image);
		if (!candidateControls || *candidateControls > maximumBytes - external ||
			retained > maximumBytes - external - *candidateControls)
			return fail("source image admission capture exceeds byte budget");
		retained += *candidateControls;
		const uint64_t payloadAvailable = maximumBytes - external;
		for (const auto &image : candidate.Frames) {
			if (image.Pixels.capacity() > payloadAvailable - retained)
				return fail("source image admission frames exceed byte budget");
			retained += image.Pixels.capacity();
		}
		if (encodeCache) {
			const uint64_t copySlots = candidate.Frames.size() * sizeof(bake::SpriteCacheFrame);
			uint64_t pixelBytes = 0;
			for (const auto &image : candidate.Frames)
				pixelBytes += image.Pixels.size();
			if (copySlots > payloadAvailable - retained ||
				pixelBytes > payloadAvailable - retained - copySlots)
				return fail("source cache encoder input overlaps exceed byte budget");
			std::vector<bake::SpriteCacheFrame> bytes;
			bytes.reserve(candidate.Frames.size());
			if (bytes.capacity() > candidate.Frames.size())
				return fail("source cache encoder table exceeds admission");
			for (const auto &image : candidate.Frames)
				bytes.push_back({image.Width, image.Height, image.Pixels});
			uint64_t copies = bytes.capacity() * sizeof(bake::SpriteCacheFrame);
			for (const auto &frame : bytes)
				copies += frame.Rgba.capacity();
			if (copies > payloadAvailable - retained)
				return fail("source cache encoder retained input exceeds admission");
			std::string encoded;
			if (!bake::WriteSpriteCache(
					bytes,
					bake::SpriteCacheLayout::Rgba8TopDown,
					encoded,
					failure,
					payloadAvailable - retained - copies,
					scalar ? bake::SpriteCacheShape::Sprite : bake::SpriteCacheShape::Array
				))
				return false;
			candidate.EncodedCache = std::move(encoded);
			candidate.CacheLayout = bake::SpriteCacheLayout::Rgba8TopDown;
		}
		core::Metrics::Count("imagegraphexport.live_cache_frame_bytes", double(retained));
		core::Metrics::Count("imagegraphexport.live_cache_frames", candidate.Frames.size());
		result = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		failure = "source image admission allocation failed";
		return false;
	}
}
