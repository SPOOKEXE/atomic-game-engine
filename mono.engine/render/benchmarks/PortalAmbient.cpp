#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/render/PortalExchange.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.render.bench.portal-ambient")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr size_t BATCHES = 8;
	bool DirectionalFixture() {
		const char *value = std::getenv("MONO_PORTAL_CODEC_DIRECTIONAL");
		return value && std::string_view(value) == "1";
	}

	const char *FixtureLabel(const char *ambient, const char *directional) {
		return DirectionalFixture() ? directional : ambient;
	}
	enum class Operation { DecodeRaw, DecodeCompressed, RoundTrip };

	void Require(bool valid, const std::string &error = {}) {
		if (!valid) throw std::runtime_error("portal ambient benchmark validation failed: " + error);
	}

	PortalImageReply Pattern(size_t view, uint32_t extent, bool directional) {
		PortalImageReply reply;
		reply.Key = {view + 1, "Workspace/Portal" + std::to_string(view), 2, 3};
		reply.Status = PortalImageStatus::Ok;
		reply.Scope = PortalImageScope::OpaqueLighting;
		reply.CaptureTick = 4;
		reply.ContentRevision = 5;
		reply.LightingRevision = 6;
		reply.Width = reply.Height = extent;
		reply.RowStride = extent * 8;
		reply.CaptureLighting.emplace();
		core::ByteWriter colour, depth, normal, response, baseline, directionalResponse;
		for (uint32_t y = 0; y < extent; ++y) {
			for (uint32_t x = 0; x < extent; ++x) {
				const uint32_t tile = (x / 8 + 13 * (y / 8) + 7 * view) % 64;
				depth.WriteFloat(2.f + float(x) / 128 + float(y) / 256 + float(view) / 16);
				normal.WriteUInt32((256 + tile * 8) | ((384 + tile * 4) << 10) | (1023u << 20) | (3u << 30));
				for (uint32_t channel = 0; channel < 4; ++channel) {
					const uint32_t pattern = (tile * 17 + channel * 137 + x % 8) % 1024;
					colour.WriteUInt16(channel == 3 ? 0x3c00 : uint16_t(0x3400 + pattern));
					response.WriteFloat(channel == 3 ? .75f : .125f + float(pattern) / 2048);
					baseline.WriteFloat(channel == 3 ? 1.f : .5f + float(pattern) / 128);
					if (directional)
						directionalResponse.WriteFloat(
							channel == 3 ? float(tile % 5) / 4 : .25f + float(pattern) / 256
						);
				}
			}
		}
		const std::array writers{&colour, &depth, &normal, &response, &baseline, &directionalResponse};
		const std::array planes{
			&reply.Pixels,
			&reply.Depth,
			&reply.Normal,
			&reply.AmbientResponse,
			&reply.LightingBaseline,
			&reply.DirectionalResponse
		};
		const std::array hashes{
			&reply.PixelHash,
			&reply.DepthHash,
			&reply.NormalHash,
			&reply.AmbientResponseHash,
			&reply.LightingBaselineHash,
			&reply.DirectionalResponseHash
		};
		for (size_t plane = 0; plane < planes.size(); ++plane) {
			if (writers[plane]->Size() == 0) continue;
			planes[plane]->assign(writers[plane]->Bytes().begin(), writers[plane]->Bytes().end());
			*hashes[plane] = assets::Hasher::Of(*planes[plane]);
		}
		return reply;
	}

	// Preserve the encoder's metadata prefix; replace only its compressed
	// payload records. This isolates raw decoding without adding a production toggle.
	std::vector<std::byte> RawPacket(const PortalImageReply &reply, std::span<const std::byte> encoded) {
		core::ByteReader reader(encoded);
		reader.ReadUInt32();
		Require(reader.ReadUInt16() == 19);
		Require(reader.ReadUInt8() == 2 && reader.ReadUInt8() == 0);
		reader.ReadUInt64();
		reader.ReadString();
		reader.ReadUInt64();
		reader.ReadUInt64();
		reader.ReadUInt8();
		reader.ReadUInt8();
		const size_t flagsOffset = reader.Position();
		const auto flags = reader.ReadUInt16();
		Require((flags & (2 | 8 | 16)) == (2 | 8 | 16));
		reader.ReadString();
		reader.ReadRawView(3 * 8 + 3 * 4);
		// The fixture has the seventeen scalar lighting floats and no local lights.
		reader.ReadRawView(17 * 4);
		Require(reader.ReadUInt8() == 0 && !reader.Failed());
		std::vector<std::byte> prefix(encoded.begin(), encoded.begin() + reader.Position());
		const uint16_t rawFlags = flags & ~uint16_t(1 | 4 | 32 | 64 | 128 | 512);
		prefix[flagsOffset] = std::byte(rawFlags & 255);
		prefix[flagsOffset + 1] = std::byte(rawFlags >> 8);
		core::ByteWriter writer;
		writer.WriteRaw(prefix.data(), prefix.size());
		const std::array planes{
			&reply.Pixels,
			&reply.Depth,
			&reply.Normal,
			&reply.AmbientResponse,
			&reply.LightingBaseline,
			&reply.DirectionalResponse
		};
		const std::array hashes{
			&reply.PixelHash,
			&reply.DepthHash,
			&reply.NormalHash,
			&reply.AmbientResponseHash,
			&reply.LightingBaselineHash,
			&reply.DirectionalResponseHash
		};
		for (size_t plane = 0; plane < planes.size(); ++plane) {
			if (planes[plane]->empty()) continue;
			writer.WriteRaw(hashes[plane]->Digest.data(), hashes[plane]->Digest.size());
			writer.WriteUInt32(static_cast<uint32_t>(planes[plane]->size()));
			writer.WriteRaw(planes[plane]->data(), planes[plane]->size());
		}
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	void Verify(const PortalImageReply &decoded, const PortalImageReply &expected) {
		Require(
			decoded.Key == expected.Key && decoded.Status == expected.Status &&
			decoded.Scope == expected.Scope && decoded.CaptureTick == expected.CaptureTick &&
			decoded.ContentRevision == expected.ContentRevision &&
			decoded.LightingRevision == expected.LightingRevision && decoded.Width == expected.Width &&
			decoded.Height == expected.Height && decoded.RowStride == expected.RowStride &&
			decoded.CaptureLighting == expected.CaptureLighting && decoded.Diagnostic == expected.Diagnostic
		);
		const std::array planes{
			&decoded.Pixels,
			&decoded.Depth,
			&decoded.Normal,
			&decoded.AmbientResponse,
			&decoded.LightingBaseline,
			&decoded.DirectionalResponse
		};
		const std::array expectedPlanes{
			&expected.Pixels,
			&expected.Depth,
			&expected.Normal,
			&expected.AmbientResponse,
			&expected.LightingBaseline,
			&expected.DirectionalResponse
		};
		const std::array hashes{
			decoded.PixelHash,
			decoded.DepthHash,
			decoded.NormalHash,
			decoded.AmbientResponseHash,
			decoded.LightingBaselineHash,
			decoded.DirectionalResponseHash
		};
		const std::array expectedHashes{
			expected.PixelHash,
			expected.DepthHash,
			expected.NormalHash,
			expected.AmbientResponseHash,
			expected.LightingBaselineHash,
			expected.DirectionalResponseHash
		};
		for (size_t plane = 0; plane < planes.size(); ++plane) {
			Require(
				planes[plane]->size() == expectedPlanes[plane]->size() &&
				hashes[plane] == expectedHashes[plane]
			);
			testing::Consume(hashes[plane]);
		}
	}

	void OneView(const PortalImageReply &expected, std::span<const std::byte> wire, Operation operation) {
		std::vector<std::byte> encoded;
		std::string error;
		if (operation == Operation::RoundTrip) {
			Require(EncodePortalImageReply(expected, encoded, error), error);
			wire = encoded;
		}
		PortalImageReply decoded;
		Require(DecodePortalImageReply(wire, decoded, error), error);
		Verify(decoded, expected);
		testing::Consume(wire.size());
	}

	void Profile(
		const PortalImageReply &expected,
		std::span<const std::byte> wire,
		Operation operation,
		std::string_view label
	) {
		const bool previouslyEnabled = core::FrameGraph::IsEnabled();
		core::FrameGraph::SetEnabled(true);
		core::FrameGraph::BeginFrame();
		try {
			{
				core::FrameGraph::Scope root(label, core::ProfileCategory::Engine);
				OneView(expected, wire, operation);
			}
		} catch (...) {
			core::FrameGraph::EndFrame();
			core::FrameGraph::SetEnabled(previouslyEnabled);
			throw;
		}
		core::FrameGraph::EndFrame();
		std::cout << "PIMG19 FrameGraph warmup, 1 view | " << label
				  << " frame_ms=" << core::FrameGraph::FrameMilliseconds()
				  << " unmarked_ms=" << core::FrameGraph::UnmarkedMilliseconds()
				  << " dropped=" << core::FrameGraph::Dropped() << '\n';
		for (const auto &span : core::FrameGraph::Spans())
			std::cout << std::string(span.Depth * 2, ' ') << span.Name
					  << " start_ms=" << span.StartMilliseconds << " inclusive_ms=" << span.Milliseconds
					  << " self_ms=" << span.SelfMilliseconds << '\n';
		core::FrameGraph::SetEnabled(previouslyEnabled);
	}

	struct Messages {
		std::array<PortalImageReply, 8> Images;
		std::array<std::vector<std::byte>, 8> Compressed, Raw;
		Messages() {
			const bool directional = DirectionalFixture();
			const uint32_t extent = directional ? 255 : 256;
			const size_t rawBytes = size_t(extent) * extent * (directional ? 64 : 48);
			if (directional) {
				const auto oversized = Pattern(0, 256, true);
				std::vector<std::byte> compressed;
				std::string error;
				Require(EncodePortalImageReply(oversized, compressed, error), error);
				PortalImageReply decoded;
				Require(DecodePortalImageReply(compressed, decoded, error), error);
				Require(decoded == oversized);
				const auto raw = RawPacket(oversized, compressed);
				Require(raw.size() > MAX_PORTAL_EXCHANGE_BYTES);
				Require(!DecodePortalImageReply(raw, decoded, error) && !error.empty());
				std::cout << "PIMG19 six-plane 256x256 admission: raw_refused_bytes=" << raw.size()
						  << " compressed_accepted_bytes=" << compressed.size() << '\n';
			}
			for (size_t view = 0; view < Images.size(); ++view) {
				Images[view] = Pattern(view, extent, directional);
				std::string error;
				Require(EncodePortalImageReply(Images[view], Compressed[view], error), error);
				Raw[view] = RawPacket(Images[view], Compressed[view]);
				Require(Compressed[view].size() < Raw[view].size());
				Require(Raw[view].size() <= MAX_PORTAL_EXCHANGE_BYTES);
				for (const auto *wire : {&Compressed[view], &Raw[view]}) {
					PortalImageReply decoded;
					Require(DecodePortalImageReply(*wire, decoded, error), error);
					Require(decoded == Images[view]);
				}
			}
			for (size_t views : {1, 2, 8}) {
				size_t compressed = 0, raw = 0;
				for (size_t view = 0; view < views; ++view) {
					compressed += Compressed[view].size();
					raw += Raw[view].size();
				}
				std::cout << "PIMG19 CPU codec views=" << views << " extent=" << extent
						  << " planes=" << (directional ? 6 : 5) << " plane_bytes=" << rawBytes * views
						  << " raw_wire_bytes=" << raw << " compressed_wire_bytes=" << compressed << '\n';
			}
			const char *profile = std::getenv("MONO_PORTAL_CODEC_PROFILE");
			if (profile && std::string_view(profile) == "1") {
				Profile(Images[0], Raw[0], Operation::DecodeRaw, "raw decode+verify");
				Profile(Images[0], Compressed[0], Operation::DecodeCompressed, "compressed decode+verify");
				Profile(Images[0], Compressed[0], Operation::RoundTrip, "auto encode+decode+verify");
			}
		}
	};

	void Run(size_t views, Operation operation) {
		static const Messages messages;
		for (size_t batch = 0; batch < BATCHES; ++batch) {
			for (size_t view = 0; view < views; ++view) {
				OneView(
					messages.Images[view],
					operation == Operation::DecodeRaw ? messages.Raw[view] : messages.Compressed[view],
					operation
				);
			}
		}
	}
}

BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | raw decode+verify | 1 view batch",
		"PIMG19 255x255 64Bpx | raw decode+verify | 1 view batch"
	),
	BATCHES
) {
	Run(1, Operation::DecodeRaw);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | raw decode+verify | 2 view batch",
		"PIMG19 255x255 64Bpx | raw decode+verify | 2 view batch"
	),
	BATCHES
) {
	Run(2, Operation::DecodeRaw);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | raw decode+verify | 8 view batch",
		"PIMG19 255x255 64Bpx | raw decode+verify | 8 view batch"
	),
	BATCHES
) {
	Run(8, Operation::DecodeRaw);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | compressed decode+verify | 1 view batch",
		"PIMG19 255x255 64Bpx | compressed decode+verify | 1 view batch"
	),
	BATCHES
) {
	Run(1, Operation::DecodeCompressed);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | compressed decode+verify | 2 view batch",
		"PIMG19 255x255 64Bpx | compressed decode+verify | 2 view batch"
	),
	BATCHES
) {
	Run(2, Operation::DecodeCompressed);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | compressed decode+verify | 8 view batch",
		"PIMG19 255x255 64Bpx | compressed decode+verify | 8 view batch"
	),
	BATCHES
) {
	Run(8, Operation::DecodeCompressed);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | auto encode+decode+verify | 1 view batch",
		"PIMG19 255x255 64Bpx | auto encode+decode+verify | 1 view batch"
	),
	BATCHES
) {
	Run(1, Operation::RoundTrip);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | auto encode+decode+verify | 2 view batch",
		"PIMG19 255x255 64Bpx | auto encode+decode+verify | 2 view batch"
	),
	BATCHES
) {
	Run(2, Operation::RoundTrip);
}
BENCH(
	FixtureLabel(
		"PIMG19 256x256 48Bpx | auto encode+decode+verify | 8 view batch",
		"PIMG19 255x255 64Bpx | auto encode+decode+verify | 8 view batch"
	),
	BATCHES
) {
	Run(8, Operation::RoundTrip);
}
