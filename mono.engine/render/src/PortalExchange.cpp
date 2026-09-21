#include "PortalImageSamples.hpp"
#include "PortalLayerPreflight.hpp"
#include "PortalPlayerIdentity.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalExchange.hpp>
#include <engine/render/PortalGeometry.hpp>

#include <cmath>
#include <utility>
#include <zstd.h>

namespace engine::render {
	namespace {
		constexpr uint32_t MAGIC = 0x474d4950; // PIMG
		constexpr uint16_t VERSION = 19;
		constexpr size_t MAX_KEY = 256;
		constexpr size_t MAX_DIAGNOSTIC = 1024;
		static_assert(
			size_t(MAX_PORTAL_IMAGE_PIXELS) * 12 + MAX_KEY + MAX_DIAGNOSTIC + 1000 < MAX_PORTAL_EXCHANGE_BYTES
		);

		bool Refuse(std::string &error, const char *reason) {
			error = reason;
			return false;
		}
		bool Text(std::string_view text, size_t maximum, bool empty = false) {
			return (empty || !text.empty()) && text.size() <= maximum &&
				   text.find('\0') == std::string_view::npos;
		}
		bool ValidKey(const PortalExchangeKey &key) {
			return key.RequestId != 0 && Text(key.PortalKey, MAX_KEY);
		}
		template <size_t N> bool Finite(const std::array<float, N> &values) {
			for (float value : values) {
				if (!std::isfinite(value)) {
					return false;
				}
			}
			return true;
		}
		bool Unit(std::span<const float> values) {
			double norm = 0;
			for (float value : values) {
				norm += double(value) * value;
			}
			return std::abs(norm - 1) <= 0.001;
		}
		bool Extent(uint32_t width, uint32_t height) {
			return width > 0 && height > 0 && width <= MAX_PORTAL_IMAGE_EXTENT &&
				   height <= MAX_PORTAL_IMAGE_EXTENT;
		}
		bool ValidRequest(const PortalImageRequest &request) {
			if (request.Projection > PortalImageProjection::Eye) return false;
			if (request.OrderedLayers &&
				(request.Scope != PortalImageScope::OpaqueLighting ||
				 request.RecursionDepth > MAX_PORTAL_CAPTURE_TREE_DEPTH || request.KnownImage))
				return false;
			const bool eye = request.Projection == PortalImageProjection::Eye;
			if (!ValidPortalPlayerIdentity(request.EyePlayer) ||
				!ValidPortalPlayerIdentity(request.RetainedBodyPlayer) ||
				(!request.RetainedBodyPlayer.empty() && !request.OrderedLayers))
				return false;
			if (eye && (request.Entrance || request.ClipPlane != std::array<float, 4>{})) return false;
			if (request.Entrance) {
				const auto &entrance = *request.Entrance;
				if (!Text(entrance.SourceWorld, MAX_KEY) || !Finite(entrance.Centre) ||
					!Finite(entrance.First) || !Finite(entrance.Second))
					return false;
				double first = 0, second = 0, dot = 0;
				for (size_t axis = 0; axis < 3; ++axis) {
					first += double(entrance.First[axis]) * entrance.First[axis];
					second += double(entrance.Second[axis]) * entrance.Second[axis];
					dot += double(entrance.First[axis]) * entrance.Second[axis];
				}
				if (first <= 0 || second <= 0 || std::abs(dot) > .001 * std::sqrt(first * second))
					return false;
			}
			if (!request.Geometry.empty()) {
				PortalGeometry geometry;
				std::string error;
				if (!DecodePortalGeometry(request.Geometry, geometry, error)) {
					return false;
				}
			}
			const auto &lens = request.Frustum;
			return ValidKey(request.Key) && request.Scope <= PortalImageScope::OpaqueLighting &&
				   Finite(request.Position) && Finite(request.Orientation) && Unit(request.Orientation) &&
				   Finite(lens) && lens[0] < lens[1] && lens[2] < lens[3] && lens[4] > 0 &&
				   lens[5] > lens[4] && Finite(request.ClipPlane) &&
				   (eye || Unit(std::span(request.ClipPlane).first<3>())) &&
				   Extent(request.Width, request.Height) &&
				   request.RecursionDepth <= MAX_PORTAL_IMAGE_RECURSION &&
				   request.PixelBudget <= MAX_PORTAL_IMAGE_PIXELS &&
				   request.PixelBudget >=
					   uint64_t(request.Width) * request.Height * (request.OrderedLayers ? 4 : 1);
		}
		bool FinitePixels(std::span<const std::byte> pixels) {
			ENGINE_PROFILE("portal image half samples");
			for (size_t index = 0; index < pixels.size(); index += 2) {
				// Half-float exponent 31 denotes either infinity or NaN.
				if ((std::to_integer<uint8_t>(pixels[index + 1]) & 0x7c) == 0x7c) {
					return false;
				}
			}
			return true;
		}
		bool ValidCaptureLighting(const PortalCaptureLighting *lighting) {
			if (!lighting) return true;
			if (lighting->LightCount > MAX_PORTAL_CAPTURE_LIGHTS || !Finite(lighting->FogColour) ||
				!std::isfinite(lighting->FogStart) || !std::isfinite(lighting->FogEnd) ||
				lighting->FogStart < 0 || lighting->FogEnd <= lighting->FogStart)
				return false;
			for (const float value : lighting->FogColour)
				if (value < 0) return false;
			for (size_t index = 0; index < lighting->Lights.size(); ++index) {
				const auto &light = lighting->Lights[index];
				if (index >= lighting->LightCount) {
					if (light != PortalCaptureLight{}) return false;
					continue;
				}
				if (!Finite(light.Position) || !Finite(light.Colour) || !Finite(light.Direction) ||
					!std::isfinite(light.Range) || light.Range <= 0 || !std::isfinite(light.ConeCosine) ||
					light.ConeCosine < -1 || light.ConeCosine > 1 ||
					(light.ConeCosine != -1 && !Unit(light.Direction)))
					return false;
				for (const float value : light.Colour)
					if (value < 0) return false;
			}
			if (!Finite(lighting->Direction) || !Unit(lighting->Direction)) return false;
			for (const auto &colour : {lighting->Ambient, lighting->OutdoorAmbient, lighting->Direct}) {
				if (!Finite(colour)) return false;
				for (const float value : colour)
					if (value < 0) return false;
			}
			return true;
		}
		bool ValidAmbient(
			const PortalImageReply &reply,
			std::span<const std::byte> normal,
			std::span<const std::byte> response,
			std::span<const std::byte> baseline,
			std::span<const std::byte> directional
		) {
			if (directional.empty()) {
				if (!reply.DirectionalResponseHash.IsZero()) return false;
			} else {
				if (normal.empty() || directional.size() != size_t(reply.Width) * reply.Height * 16)
					return false;
				{
					ENGINE_PROFILE("portal image directional hash");
					if (assets::Hasher::Of(directional) != reply.DirectionalResponseHash) return false;
				}
				ENGINE_PROFILE("portal image directional samples");
				if (!ValidPortalResponseSamples(directional)) return false;
			}
			if (normal.empty() && response.empty() && baseline.empty())
				return reply.NormalHash.IsZero() && reply.AmbientResponseHash.IsZero() &&
					   reply.LightingBaselineHash.IsZero();
			const size_t pixels = size_t(reply.Width) * reply.Height;
			if (reply.Status != PortalImageStatus::Ok || reply.Scope != PortalImageScope::OpaqueLighting ||
				!reply.CaptureLighting || normal.size() != pixels * 4 || response.size() != pixels * 16 ||
				baseline.size() != pixels * 16 || normal.empty())
				return false;
			{
				ENGINE_PROFILE("portal image ambient hashes");
				if (assets::Hasher::Of(normal) != reply.NormalHash ||
					assets::Hasher::Of(response) != reply.AmbientResponseHash ||
					assets::Hasher::Of(baseline) != reply.LightingBaselineHash)
					return false;
			}
			{
				ENGINE_PROFILE("portal image baseline samples");
				if (!ValidPortalBaselineSamples(baseline)) return false;
			}
			ENGINE_PROFILE("portal image response samples");
			return ValidPortalResponseSamples(response);
		}
		bool ValidReply(
			const PortalImageReply &reply,
			std::span<const std::byte> pixels,
			std::span<const std::byte> depth,
			std::span<const std::byte> normal,
			std::span<const std::byte> response,
			std::span<const std::byte> baseline,
			std::span<const std::byte> directional
		) {
			ENGINE_PROFILE("portal image validation");
			if (!ValidAmbient(reply, normal, response, baseline, directional) ||
				(!normal.empty() && depth.empty()) || !ValidKey(reply.Key) ||
				reply.Scope > PortalImageScope::OpaqueLighting ||
				!Text(reply.Diagnostic, MAX_DIAGNOSTIC, reply.Status == PortalImageStatus::Ok) ||
				!ValidCaptureLighting(reply.CaptureLighting ? &*reply.CaptureLighting : nullptr)) {
				return false;
			}
			if (reply.Status > PortalImageStatus::Failed) {
				return false;
			}
			if (reply.Status != PortalImageStatus::Ok) {
				return reply.Width == 0 && reply.Height == 0 && reply.RowStride == 0 &&
					   !reply.CaptureLighting && reply.PixelHash.IsZero() && pixels.empty() &&
					   reply.DepthHash.IsZero() && depth.empty();
			}
			if (!Extent(reply.Width, reply.Height) || reply.RowStride != reply.Width * 8 ||
				pixels.size() != size_t(reply.RowStride) * reply.Height) {
				return false;
			}
			if (depth.empty()) {
				if (!reply.DepthHash.IsZero()) return false;
			} else {
				if (depth.size() != size_t(reply.Width) * reply.Height * 4) return false;
				{
					ENGINE_PROFILE("portal image depth samples");
					if (!ValidPortalDepthSamples(depth)) return false;
				}
				ENGINE_PROFILE("portal image depth hash");
				if (assets::Hasher::Of(depth) != reply.DepthHash) return false;
			}
			if (!FinitePixels(pixels)) return false;
			ENGINE_PROFILE("portal image colour hash");
			return assets::Hasher::Of(pixels) == reply.PixelHash;
		}
		bool ValidReply(
			const PortalImageReply &reply, std::span<const std::byte> pixels, std::span<const std::byte> depth
		) {
			return ValidReply(
				reply,
				pixels,
				depth,
				reply.Normal,
				reply.AmbientResponse,
				reply.LightingBaseline,
				reply.DirectionalResponse
			);
		}
		void Header(core::ByteWriter &writer, uint8_t kind) {
			writer.WriteUInt32(MAGIC);
			writer.WriteUInt16(VERSION);
			writer.WriteUInt8(kind);
			writer.WriteUInt8(0);
		}
		bool Header(core::ByteReader &reader, uint8_t kind) {
			return reader.ReadUInt32() == MAGIC && reader.ReadUInt16() == VERSION &&
				   reader.ReadUInt8() == kind && reader.ReadUInt8() == 0 && !reader.Failed();
		}
		void Key(core::ByteWriter &writer, const PortalExchangeKey &key) {
			writer.WriteUInt64(key.RequestId);
			writer.WriteString(key.PortalKey);
			writer.WriteUInt64(key.CameraRevision);
			writer.WriteUInt64(key.SeamRevision);
		}
		bool Key(core::ByteReader &reader, PortalExchangeKey &key) {
			key.RequestId = reader.ReadUInt64();
			const auto text = reader.ReadString();
			if (!Text(text, MAX_KEY)) {
				return false;
			}
			key.PortalKey = text;
			key.CameraRevision = reader.ReadUInt64();
			key.SeamRevision = reader.ReadUInt64();
			return !reader.Failed() && ValidKey(key);
		}
		template <size_t N> void Floats(core::ByteWriter &writer, const std::array<float, N> &values) {
			for (float value : values) {
				writer.WriteFloat(value);
			}
		}
		template <size_t N> void Floats(core::ByteReader &reader, std::array<float, N> &values) {
			for (float &value : values) {
				value = reader.ReadFloat();
			}
		}
		bool ValidCaptureLens(const PortalCaptureLens &lens, std::string_view shader) {
			return Text(shader, MAX_KEY) && !lens.ProgramHash.IsZero() && Finite(lens.Position) &&
				   Finite(lens.Orientation) && Unit(lens.Orientation) && lens.Shape == 0 &&
				   std::isfinite(lens.Radius) && lens.Radius > 0 && std::isfinite(lens.InnerRadius) &&
				   lens.InnerRadius >= 0 && lens.InnerRadius <= lens.Radius && std::isfinite(lens.Falloff) &&
				   lens.Falloff >= 0 && lens.Falloff <= 1 && std::isfinite(lens.Strength) &&
				   lens.Strength >= 0 && std::isfinite(lens.Spin);
		}
		bool ValidLensProgram(const assets::ContentHash &hash, std::span<const std::byte> code) {
			if (code.size() < 5 * sizeof(uint32_t) || code.size() % sizeof(uint32_t) != 0 ||
				code.size() > MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES || hash.IsZero())
				return false;
			core::ByteReader header(code);
			return header.ReadUInt32() == 0x07230203 && assets::Hasher::Of(code) == hash;
		}
		void CaptureLenses(core::ByteWriter &writer, const PortalCaptureLenses &lenses) {
			writer.WriteUInt8(static_cast<uint8_t>(lenses.Entries.size()));
			writer.WriteFloat(lenses.TimeSeconds);
			writer.WriteUInt8(static_cast<uint8_t>(lenses.Programs.size()));
			for (const auto &program : lenses.Programs) {
				writer.WriteRaw(program.Hash.Digest.data(), program.Hash.Digest.size());
				writer.WriteUInt32(static_cast<uint32_t>(program.SpirV.size()));
				for (const auto word : program.SpirV)
					writer.WriteUInt32(word);
			}
			for (const auto &lens : lenses.Entries) {
				Floats(writer, lens.Position);
				Floats(writer, lens.Orientation);
				writer.WriteString(lens.Shader);
				writer.WriteRaw(lens.ProgramHash.Digest.data(), lens.ProgramHash.Digest.size());
				writer.WriteFloat(lens.Radius);
				writer.WriteFloat(lens.InnerRadius);
				writer.WriteFloat(lens.Falloff);
				writer.WriteFloat(lens.Strength);
				writer.WriteFloat(lens.Spin);
				writer.WriteInt32(lens.Priority);
				writer.WriteUInt8(lens.Shape);
			}
		}
		// A null destination validates borrowed text and code without allocating before inbox admission.
		bool CaptureLenses(core::ByteReader &reader, PortalCaptureLenses *out, size_t &bytes) {
			const auto count = reader.ReadUInt8();
			const auto time = reader.ReadFloat();
			if (reader.Failed() || count > MAX_PORTAL_CAPTURE_LENSES || !std::isfinite(time) ||
				(count == 0 && (time != 0 || std::signbit(time))))
				return false;
			const auto programCount = reader.ReadUInt8();
			if (reader.Failed() || programCount > MAX_PORTAL_CAPTURE_LENSES || programCount > count)
				return false;
			struct BorrowedProgram {
				assets::ContentHash Hash;
				std::span<const std::byte> Code;
				bool Used = false;
			};
			std::array<BorrowedProgram, MAX_PORTAL_CAPTURE_LENSES> programs{};
			size_t codeBytes = 0;
			for (size_t index = 0; index < programCount; ++index) {
				auto &program = programs[index];
				reader.ReadRaw(program.Hash.Digest.data(), program.Hash.Digest.size());
				const auto words = reader.ReadUInt32();
				if (reader.Failed() || words > (MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES - codeBytes) / 4)
					return false;
				program.Code = reader.ReadRawView(size_t(words) * 4);
				if (reader.Failed() || !ValidLensProgram(program.Hash, program.Code)) return false;
				for (size_t previous = 0; previous < index; ++previous)
					if (programs[previous].Hash == program.Hash) return false;
				codeBytes += program.Code.size();
			}
			bytes = size_t(count) * sizeof(PortalCaptureLens) +
					size_t(programCount) * sizeof(PortalCaptureLensProgram) + codeBytes;
			if (out) {
				out->TimeSeconds = time;
				out->Entries.reserve(count);
			}
			for (size_t index = 0; index < count; ++index) {
				PortalCaptureLens lens;
				Floats(reader, lens.Position);
				Floats(reader, lens.Orientation);
				const auto shader = reader.ReadString();
				reader.ReadRaw(lens.ProgramHash.Digest.data(), lens.ProgramHash.Digest.size());
				lens.Radius = reader.ReadFloat();
				lens.InnerRadius = reader.ReadFloat();
				lens.Falloff = reader.ReadFloat();
				lens.Strength = reader.ReadFloat();
				lens.Spin = reader.ReadFloat();
				lens.Priority = reader.ReadInt32();
				lens.Shape = reader.ReadUInt8();
				if (reader.Failed() || !ValidCaptureLens(lens, shader)) return false;
				if (programCount != 0) {
					bool covered = false;
					for (size_t program = 0; program < programCount; ++program) {
						if (programs[program].Hash != lens.ProgramHash) continue;
						programs[program].Used = true;
						covered = true;
						break;
					}
					if (!covered) return false;
				}
				bytes += shader.size();
				if (out) {
					lens.Shader = shader;
					out->Entries.push_back(std::move(lens));
				}
			}
			for (size_t index = 0; index < programCount; ++index)
				if (!programs[index].Used) return false;
			if (out) {
				out->Programs.reserve(programCount);
				for (size_t index = 0; index < programCount; ++index) {
					PortalCaptureLensProgram program;
					program.Hash = programs[index].Hash;
					core::ByteReader code(programs[index].Code);
					program.SpirV.reserve(programs[index].Code.size() / 4);
					while (!code.AtEnd())
						program.SpirV.push_back(code.ReadUInt32());
					out->Programs.push_back(std::move(program));
				}
			}
			return true;
		}
		void CaptureLighting(core::ByteWriter &writer, const PortalCaptureLighting &light) {
			Floats(writer, light.Direction);
			Floats(writer, light.Ambient);
			Floats(writer, light.OutdoorAmbient);
			Floats(writer, light.Direct);
			Floats(writer, light.FogColour);
			writer.WriteFloat(light.FogStart);
			writer.WriteFloat(light.FogEnd);
			writer.WriteUInt8(light.LightCount);
			for (size_t index = 0; index < light.LightCount; ++index) {
				const auto &local = light.Lights[index];
				Floats(writer, local.Position);
				writer.WriteFloat(local.Range);
				Floats(writer, local.Colour);
				Floats(writer, local.Direction);
				writer.WriteFloat(local.ConeCosine);
			}
		}
		void CaptureLighting(core::ByteReader &reader, PortalCaptureLighting &light) {
			Floats(reader, light.Direction);
			Floats(reader, light.Ambient);
			Floats(reader, light.OutdoorAmbient);
			Floats(reader, light.Direct);
			Floats(reader, light.FogColour);
			light.FogStart = reader.ReadFloat();
			light.FogEnd = reader.ReadFloat();
			light.LightCount = reader.ReadUInt8();
			if (light.LightCount > MAX_PORTAL_CAPTURE_LIGHTS) return;
			for (size_t index = 0; index < light.LightCount; ++index) {
				auto &local = light.Lights[index];
				Floats(reader, local.Position);
				local.Range = reader.ReadFloat();
				Floats(reader, local.Colour);
				Floats(reader, local.Direction);
				local.ConeCosine = reader.ReadFloat();
			}
		}
		void Commit(
			const core::ByteWriter &writer,
			std::vector<std::byte> &out,
			std::string &error,
			std::span<const std::byte> pixels = {}
		) {
			// Keep image bytes out of the header writer, avoiding a second full image allocation/copy.
			std::vector<std::byte> encoded;
			encoded.reserve(writer.Size() + pixels.size());
			encoded.insert(encoded.end(), writer.Bytes().begin(), writer.Bytes().end());
			encoded.insert(encoded.end(), pixels.begin(), pixels.end());
			out = std::move(encoded);
			error.clear();
		}
	}

	bool ValidPortalCaptureLighting(const PortalCaptureLighting &lighting) {
		return ValidCaptureLighting(&lighting);
	}

	static bool EncodeReceipt(
		const PortalResidentReceipt &receipt, std::vector<std::byte> &out, std::string &error, uint8_t kind
	) {
		if (!ValidKey(receipt.Key) || receipt.Scope > PortalImageScope::OpaqueLighting ||
			!Extent(receipt.Width, receipt.Height) ||
			!ValidCaptureLighting(receipt.CaptureLighting ? &*receipt.CaptureLighting : nullptr)) {
			return Refuse(error, "invalid resident portal receipt");
		}
		core::ByteWriter writer;
		Header(writer, kind);
		Key(writer, receipt.Key);
		writer.WriteUInt8(static_cast<uint8_t>(receipt.Scope));
		writer.WriteUInt8(receipt.CaptureLighting ? 1 : 0);
		writer.WriteUInt16(0);
		writer.WriteUInt64(receipt.CaptureTick);
		writer.WriteUInt64(receipt.ContentRevision);
		writer.WriteUInt64(receipt.LightingRevision);
		writer.WriteUInt32(receipt.Width);
		writer.WriteUInt32(receipt.Height);
		if (receipt.CaptureLighting) CaptureLighting(writer, *receipt.CaptureLighting);
		Commit(writer, out, error);
		return true;
	}
	static bool DecodeReceipt(
		std::span<const std::byte> bytes, PortalResidentReceipt &out, std::string &error, uint8_t kind
	) {
		if (bytes.size() > MAX_KEY + 900) {
			return Refuse(error, "resident portal receipt too large");
		}
		core::ByteReader reader(bytes);
		PortalResidentReceipt receipt;
		if (!Header(reader, kind) || !Key(reader, receipt.Key)) {
			return Refuse(error, "invalid resident portal header");
		}
		receipt.Scope = static_cast<PortalImageScope>(reader.ReadUInt8());
		const auto hasCaptureLighting = reader.ReadUInt8();
		if (hasCaptureLighting > 1 || reader.ReadUInt16() != 0) {
			return Refuse(error, "noncanonical resident portal padding");
		}
		receipt.CaptureTick = reader.ReadUInt64();
		receipt.ContentRevision = reader.ReadUInt64();
		receipt.LightingRevision = reader.ReadUInt64();
		receipt.Width = reader.ReadUInt32();
		receipt.Height = reader.ReadUInt32();
		if (hasCaptureLighting) CaptureLighting(reader, receipt.CaptureLighting.emplace());
		if (reader.Failed() || !reader.AtEnd() || receipt.Scope > PortalImageScope::OpaqueLighting ||
			!Extent(receipt.Width, receipt.Height) ||
			!ValidCaptureLighting(receipt.CaptureLighting ? &*receipt.CaptureLighting : nullptr)) {
			return Refuse(error, "invalid resident portal receipt");
		}
		out = std::move(receipt);
		error.clear();
		return true;
	}

	bool EncodePortalResidentReceipt(
		const PortalResidentReceipt &receipt, std::vector<std::byte> &out, std::string &error
	) {
		return EncodeReceipt(receipt, out, error, 3);
	}
	bool DecodePortalResidentReceipt(
		std::span<const std::byte> bytes, PortalResidentReceipt &out, std::string &error
	) {
		return DecodeReceipt(bytes, out, error, 3);
	}
	bool EncodePortalImageRenewal(
		const PortalResidentReceipt &receipt, std::vector<std::byte> &out, std::string &error
	) {
		return EncodeReceipt(receipt, out, error, 4);
	}
	bool DecodePortalImageRenewal(
		std::span<const std::byte> bytes, PortalResidentReceipt &out, std::string &error
	) {
		return DecodeReceipt(bytes, out, error, 4);
	}

	template <class KeyType>
	static std::optional<PortalImageReplyMatch> MatchReply(
		std::span<const std::byte> bytes,
		const KeyType &expected,
		uint32_t width,
		uint32_t height,
		PortalImageScope scope
	) {
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES ||
			(expected.RequestId == 0 || !Text(expected.PortalKey, MAX_KEY)) || !Extent(width, height)) {
			return {};
		}
		core::ByteReader reader(bytes);
		if (!Header(reader, 2)) {
			return {};
		}
		const uint64_t requestId = reader.ReadUInt64();
		const auto portal = reader.ReadString();
		const uint64_t cameraRevision = reader.ReadUInt64();
		const uint64_t seamRevision = reader.ReadUInt64();
		if (requestId != expected.RequestId || portal != expected.PortalKey ||
			cameraRevision != expected.CameraRevision || seamRevision != expected.SeamRevision) {
			return {};
		}
		const auto status = static_cast<PortalImageStatus>(reader.ReadUInt8());
		const auto replyScope = static_cast<PortalImageScope>(reader.ReadUInt8());
		const auto encoding = reader.ReadUInt16();
		if (status > PortalImageStatus::Failed || scope > PortalImageScope::OpaqueLighting ||
			replyScope != scope || ((encoding & 16) && scope != PortalImageScope::OpaqueLighting) ||
			encoding > 1023 || ((encoding & 512) && !(encoding & 256)) ||
			((encoding & 256) && !(encoding & 16)) || ((encoding & 4) && !(encoding & 2)) ||
			((encoding & 224) && !(encoding & 16)) || ((encoding & 16) && ((encoding & 10) != 10)) ||
			(status != PortalImageStatus::Ok && encoding != 0)) {
			return {};
		}
		const auto diagnostic = reader.ReadString();
		if (!Text(diagnostic, MAX_DIAGNOSTIC, status == PortalImageStatus::Ok)) {
			return {};
		}
		const uint64_t captureTick = reader.ReadUInt64();
		reader.ReadUInt64();
		reader.ReadUInt64();
		const uint32_t imageWidth = reader.ReadUInt32();
		const uint32_t imageHeight = reader.ReadUInt32();
		if (reader.Failed() ||
			(status == PortalImageStatus::Ok && (imageWidth != width || imageHeight != height))) {
			return {};
		}
		return PortalImageReplyMatch{
			status,
			diagnostic.size(),
			captureTick,
			(encoding & 2) ? size_t(width) * height * 4 : 0,
			((encoding & 16) ? size_t(width) * height * 36 : 0) +
				((encoding & 256) ? size_t(width) * height * 16 : 0)
		};
	}

	std::optional<PortalImageReplyMatch> MatchPortalImageReply(
		std::span<const std::byte> bytes,
		const PortalExchangeKey &expected,
		uint32_t width,
		uint32_t height,
		PortalImageScope scope
	) {
		return MatchReply(bytes, expected, width, height, scope);
	}

	bool EncodePortalImageRequest(
		const PortalImageRequest &request, std::vector<std::byte> &out, std::string &error
	) {
		if (!ValidRequest(request)) {
			return Refuse(error, "invalid portal image request");
		}
		core::ByteWriter writer;
		Header(writer, 1);
		Key(writer, request.Key);
		writer.WriteUInt8(static_cast<uint8_t>(request.Scope));
		writer.WriteUInt8(static_cast<uint8_t>(request.Projection));
		writer.WriteUInt8(request.OrderedLayers);
		writer.WriteUInt8(0);
		writer.WriteString(request.EyePlayer);
		writer.WriteString(request.RetainedBodyPlayer);
		Floats(writer, request.Position);
		Floats(writer, request.Orientation);
		Floats(writer, request.Frustum);
		Floats(writer, request.ClipPlane);
		writer.WriteUInt32(request.Width);
		writer.WriteUInt32(request.Height);
		writer.WriteUInt32(request.RecursionDepth);
		writer.WriteUInt32(request.PixelBudget);
		writer.WriteUInt32(static_cast<uint32_t>(request.Geometry.size()));
		writer.WriteRaw(request.Geometry.data(), request.Geometry.size());
		writer.WriteUInt8(request.KnownImage.has_value());
		if (request.KnownImage) {
			writer.WriteUInt64(request.KnownImage->ContentRevision);
			writer.WriteUInt64(request.KnownImage->LightingRevision);
		}
		writer.WriteUInt8(request.Entrance.has_value());
		if (request.Entrance) {
			writer.WriteString(request.Entrance->SourceWorld);
			Floats(writer, request.Entrance->Centre);
			Floats(writer, request.Entrance->First);
			Floats(writer, request.Entrance->Second);
		}
		Commit(writer, out, error);
		return true;
	}

	bool
	DecodePortalImageRequest(std::span<const std::byte> bytes, PortalImageRequest &out, std::string &error) {
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES) {
			return Refuse(error, "portal request exceeds wire budget");
		}
		core::ByteReader reader(bytes);
		PortalImageRequest request;
		if (!Header(reader, 1) || !Key(reader, request.Key)) {
			return Refuse(error, "invalid portal request header");
		}
		request.Scope = static_cast<PortalImageScope>(reader.ReadUInt8());
		request.Projection = static_cast<PortalImageProjection>(reader.ReadUInt8());
		const auto ordered = reader.ReadUInt8();
		request.OrderedLayers = ordered != 0;
		if (ordered > 1 || reader.ReadUInt8() != 0) {
			return Refuse(error, "noncanonical portal request profile");
		}
		const auto eyePlayer = reader.ReadString();
		if (reader.Failed() || eyePlayer.size() > 20) return Refuse(error, "invalid eye player identity");
		request.EyePlayer = eyePlayer;
		const auto retainedBodyPlayer = reader.ReadString();
		if (reader.Failed() || retainedBodyPlayer.size() > 20)
			return Refuse(error, "invalid retained body identity");
		request.RetainedBodyPlayer = retainedBodyPlayer;
		Floats(reader, request.Position);
		Floats(reader, request.Orientation);
		Floats(reader, request.Frustum);
		Floats(reader, request.ClipPlane);
		request.Width = reader.ReadUInt32();
		request.Height = reader.ReadUInt32();
		request.RecursionDepth = reader.ReadUInt32();
		request.PixelBudget = reader.ReadUInt32();
		const uint32_t geometryBytes = reader.ReadUInt32();
		if (reader.Failed() || geometryBytes > MAX_PORTAL_GEOMETRY_BYTES ||
			geometryBytes > reader.Remaining()) {
			return Refuse(error, "invalid portal geometry length");
		}
		const auto geometry = reader.ReadRawView(geometryBytes);
		request.Geometry.assign(geometry.begin(), geometry.end());
		const auto known = reader.ReadUInt8();
		if (known > 1) return Refuse(error, "noncanonical portal image version");
		if (known) request.KnownImage = PortalImageVersion{reader.ReadUInt64(), reader.ReadUInt64()};
		const auto entrance = reader.ReadUInt8();
		if (entrance > 1) return Refuse(error, "noncanonical portal entrance");
		if (entrance) {
			const auto source = reader.ReadString();
			if (!Text(source, MAX_KEY)) return Refuse(error, "invalid portal entrance world");
			request.Entrance.emplace();
			request.Entrance->SourceWorld = source;
			Floats(reader, request.Entrance->Centre);
			Floats(reader, request.Entrance->First);
			Floats(reader, request.Entrance->Second);
		}
		if (reader.Failed() || !reader.AtEnd() || !ValidRequest(request)) {
			return Refuse(error, "invalid portal image request");
		}
		out = std::move(request);
		error.clear();
		return true;
	}

	bool
	EncodePortalImageReply(const PortalImageReply &reply, std::vector<std::byte> &out, std::string &error) {
		ENGINE_PROFILE("portal image encode");
		if (!ValidReply(reply, reply.Pixels, reply.Depth)) {
			return Refuse(error, "invalid portal image reply");
		}
		// Lossless transport preserves linear HDR samples and their existing digest.
		// Cap scratch at the raw payload size; incompressible images keep the raw path.
		std::vector<std::byte> compressed;
		std::span<const std::byte> payload = reply.Pixels;
		uint16_t encoding = 0;
		if (payload.size() >= 256) {
			ENGINE_PROFILE("portal image colour compress");
			compressed.resize(payload.size());
			const auto size =
				ZSTD_compress(compressed.data(), compressed.size(), payload.data(), payload.size(), 1);
			if (!ZSTD_isError(size) && size < payload.size()) {
				payload = std::span(compressed).first(size);
				encoding = 1;
			}
		}
		std::vector<std::byte> compressedDepth;
		std::span<const std::byte> depth = reply.Depth;
		if (!depth.empty()) encoding |= 2;
		if (depth.size() >= 256) {
			ENGINE_PROFILE("portal image depth compress");
			compressedDepth.resize(depth.size());
			const auto size =
				ZSTD_compress(compressedDepth.data(), compressedDepth.size(), depth.data(), depth.size(), 1);
			if (!ZSTD_isError(size) && size < depth.size()) {
				depth = std::span(compressedDepth).first(size);
				encoding |= 4;
			}
		}
		if (reply.CaptureLighting) encoding |= 8;
		std::array<std::vector<std::byte>, 4> ambientScratch;
		std::array<std::span<const std::byte>, 4> ambient{
			reply.Normal, reply.AmbientResponse, reply.LightingBaseline, reply.DirectionalResponse
		};
		if (!ambient[0].empty()) encoding |= 16;
		if (!ambient[3].empty()) encoding |= 256;
		for (size_t plane = 0; plane < ((encoding & 256) ? 4u : 3u); ++plane) {
			if (ambient[plane].size() < 256) continue;
			ENGINE_PROFILE("portal image auxiliary compress");
			auto &scratch = ambientScratch[plane];
			scratch.resize(ambient[plane].size());
			const auto size = ZSTD_compress(
				scratch.data(), scratch.size(), ambient[plane].data(), ambient[plane].size(), 1
			);
			if (!ZSTD_isError(size) && size < ambient[plane].size()) {
				ambient[plane] = std::span(scratch).first(size);
				encoding |= uint16_t(plane == 3 ? 512 : 32 << plane);
			}
		}
		ENGINE_PROFILE("portal image encode framing");
		core::ByteWriter writer;
		Header(writer, 2);
		Key(writer, reply.Key);
		writer.WriteUInt8(static_cast<uint8_t>(reply.Status));
		writer.WriteUInt8(static_cast<uint8_t>(reply.Scope));
		writer.WriteUInt16(encoding);
		writer.WriteString(reply.Diagnostic);
		writer.WriteUInt64(reply.CaptureTick);
		writer.WriteUInt64(reply.ContentRevision);
		writer.WriteUInt64(reply.LightingRevision);
		writer.WriteUInt32(reply.Width);
		writer.WriteUInt32(reply.Height);
		writer.WriteUInt32(reply.RowStride);
		if (reply.CaptureLighting) CaptureLighting(writer, *reply.CaptureLighting);
		writer.WriteRaw(reply.PixelHash.Digest.data(), reply.PixelHash.Digest.size());
		writer.WriteUInt32(static_cast<uint32_t>(payload.size()));
		const size_t wireBytes =
			writer.Size() + payload.size() + (depth.empty() ? 0 : 36 + depth.size()) +
			(ambient[0].empty() ? 0 : 108 + ambient[0].size() + ambient[1].size() + ambient[2].size()) +
			(ambient[3].empty() ? 0 : 36 + ambient[3].size());
		if (wireBytes > MAX_PORTAL_EXCHANGE_BYTES) return Refuse(error, "portal reply exceeds wire budget");
		writer.WriteRaw(payload.data(), payload.size());
		if (!depth.empty()) {
			writer.WriteRaw(reply.DepthHash.Digest.data(), reply.DepthHash.Digest.size());
			writer.WriteUInt32(static_cast<uint32_t>(depth.size()));
			writer.WriteRaw(depth.data(), depth.size());
		}
		if (!ambient[0].empty()) {
			const std::array hashes{
				reply.NormalHash,
				reply.AmbientResponseHash,
				reply.LightingBaselineHash,
				reply.DirectionalResponseHash
			};
			for (size_t plane = 0; plane < ((encoding & 256) ? 4u : 3u); ++plane) {
				writer.WriteRaw(hashes[plane].Digest.data(), hashes[plane].Digest.size());
				writer.WriteUInt32(static_cast<uint32_t>(ambient[plane].size()));
				writer.WriteRaw(ambient[plane].data(), ambient[plane].size());
			}
		}
		Commit(writer, out, error);
		core::Metrics::Count(
			"render.portal_codec.raw.bytes",
			static_cast<double>(
				reply.Pixels.size() + reply.Depth.size() + reply.Normal.size() +
				reply.AmbientResponse.size() + reply.LightingBaseline.size() +
				reply.DirectionalResponse.size()
			)
		);
		core::Metrics::Count("render.portal_codec.wire.bytes", static_cast<double>(out.size()));
		return true;
	}

	bool DecodePortalImageReply(std::span<const std::byte> bytes, PortalImageReply &out, std::string &error) {
		ENGINE_PROFILE("portal image decode");
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES) {
			return Refuse(error, "portal reply exceeds wire budget");
		}
		core::ByteReader reader(bytes);
		PortalImageReply reply;
		if (!Header(reader, 2) || !Key(reader, reply.Key)) {
			return Refuse(error, "invalid portal reply header");
		}
		reply.Status = static_cast<PortalImageStatus>(reader.ReadUInt8());
		reply.Scope = static_cast<PortalImageScope>(reader.ReadUInt8());
		const auto encoding = reader.ReadUInt16();
		if (encoding > 1023 || ((encoding & 512) && !(encoding & 256)) ||
			((encoding & 256) && !(encoding & 16)) || ((encoding & 4) && !(encoding & 2)) ||
			((encoding & 224) && !(encoding & 16)) || ((encoding & 16) && ((encoding & 10) != 10)) ||
			(reply.Status != PortalImageStatus::Ok && encoding != 0)) {
			return Refuse(error, "unsupported portal reply encoding");
		}
		const auto diagnostic = reader.ReadString();
		if (!Text(diagnostic, MAX_DIAGNOSTIC, true)) {
			return Refuse(error, "invalid portal reply diagnostic");
		}
		reply.Diagnostic = diagnostic;
		reply.CaptureTick = reader.ReadUInt64();
		reply.ContentRevision = reader.ReadUInt64();
		reply.LightingRevision = reader.ReadUInt64();
		reply.Width = reader.ReadUInt32();
		reply.Height = reader.ReadUInt32();
		reply.RowStride = reader.ReadUInt32();
		if (encoding & 8) CaptureLighting(reader, reply.CaptureLighting.emplace());
		if (!ValidCaptureLighting(reply.CaptureLighting ? &*reply.CaptureLighting : nullptr))
			return Refuse(error, "invalid capture lighting");
		reader.ReadRaw(reply.PixelHash.Digest.data(), reply.PixelHash.Digest.size());
		const uint32_t length = reader.ReadUInt32();
		auto pixels = reader.ReadRawView(length);
		std::span<const std::byte> depth;
		if (encoding & 2) {
			reader.ReadRaw(reply.DepthHash.Digest.data(), reply.DepthHash.Digest.size());
			const auto depthLength = reader.ReadUInt32();
			depth = reader.ReadRawView(depthLength);
			if (!Extent(reply.Width, reply.Height) || depth.empty())
				return Refuse(error, "invalid portal depth layout");
		}
		std::array<std::span<const std::byte>, 4> ambient{};
		if (encoding & 16) {
			const std::array hashes{
				&reply.NormalHash,
				&reply.AmbientResponseHash,
				&reply.LightingBaselineHash,
				&reply.DirectionalResponseHash
			};
			for (size_t plane = 0; plane < ((encoding & 256) ? 4u : 3u); ++plane) {
				reader.ReadRaw(hashes[plane]->Digest.data(), hashes[plane]->Digest.size());
				const auto length = reader.ReadUInt32();
				ambient[plane] = reader.ReadRawView(length);
			}
		}
		if (reader.Failed() || !reader.AtEnd()) return Refuse(error, "invalid portal image payload length");
		if (reply.Status == PortalImageStatus::Ok &&
			(!Extent(reply.Width, reply.Height) || reply.RowStride != reply.Width * 8))
			return Refuse(error, "invalid portal image layout");
		if (encoding & 16) {
			if (reply.Scope != PortalImageScope::OpaqueLighting)
				return Refuse(error, "ambient planes require opaque lighting");
			for (size_t plane = 0; plane < ((encoding & 256) ? 4u : 3u); ++plane) {
				const size_t expanded = size_t(reply.Width) * reply.Height * (plane == 0 ? 4 : 16);
				const auto data = ambient[plane];
				if (encoding & (plane == 3 ? 512 : 32 << plane)) {
					if (data.size() >= expanded ||
						ZSTD_getFrameContentSize(data.data(), data.size()) != expanded ||
						ZSTD_findFrameCompressedSize(data.data(), data.size()) != data.size())
						return Refuse(error, "invalid compressed ambient plane");
				} else if (data.size() != expanded)
					return Refuse(error, "invalid ambient plane length");
			}
		}
		if (encoding & 1) {
			ENGINE_PROFILE("portal image colour decompress");
			// Validate dimensions and the single frame's declared size before allocating.
			// A compressed frame cannot select a larger output or concatenate extra frames.
			if (reply.Status != PortalImageStatus::Ok || !Extent(reply.Width, reply.Height) ||
				reply.RowStride != reply.Width * 8)
				return Refuse(error, "invalid compressed portal image layout");
			const size_t expanded = size_t(reply.RowStride) * reply.Height;
			if (length >= expanded || ZSTD_getFrameContentSize(pixels.data(), pixels.size()) != expanded ||
				ZSTD_findFrameCompressedSize(pixels.data(), pixels.size()) != pixels.size())
				return Refuse(error, "invalid compressed portal image frame");
			reply.Pixels.resize(expanded);
			const auto size = ZSTD_decompress(reply.Pixels.data(), expanded, pixels.data(), pixels.size());
			if (ZSTD_isError(size) || size != expanded)
				return Refuse(error, "invalid compressed portal image samples");
			pixels = reply.Pixels;
		}
		if (encoding & 4) {
			ENGINE_PROFILE("portal image depth decompress");
			const size_t expanded = size_t(reply.Width) * reply.Height * 4;
			if (depth.size() >= expanded ||
				ZSTD_getFrameContentSize(depth.data(), depth.size()) != expanded ||
				ZSTD_findFrameCompressedSize(depth.data(), depth.size()) != depth.size())
				return Refuse(error, "invalid compressed portal depth frame");
			reply.Depth.resize(expanded);
			const auto size = ZSTD_decompress(reply.Depth.data(), expanded, depth.data(), depth.size());
			if (ZSTD_isError(size) || size != expanded)
				return Refuse(error, "invalid compressed portal depth samples");
			depth = reply.Depth;
		}
		std::array<std::vector<std::byte>, 4> expandedAmbient;
		if (encoding & 16) {
			for (size_t plane = 0; plane < ((encoding & 256) ? 4u : 3u); ++plane) {
				const size_t expanded = size_t(reply.Width) * reply.Height * (plane == 0 ? 4 : 16);
				if (!(encoding & (plane == 3 ? 512 : 32 << plane))) continue;
				ENGINE_PROFILE("portal image auxiliary decompress");
				const auto data = ambient[plane];
				expandedAmbient[plane].resize(expanded);
				const auto size =
					ZSTD_decompress(expandedAmbient[plane].data(), expanded, data.data(), data.size());
				if (ZSTD_isError(size) || size != expanded) return Refuse(error, "invalid ambient samples");
				ambient[plane] = expandedAmbient[plane];
			}
		}
		if (!ValidReply(reply, pixels, depth, ambient[0], ambient[1], ambient[2], ambient[3])) {
			return Refuse(error, "invalid portal image reply");
		}
		ENGINE_PROFILE("portal image materialise");
		// Raw input allocates only after validation; decompression is bounded by layout.
		if (!(encoding & 1)) reply.Pixels.assign(pixels.begin(), pixels.end());
		if (!(encoding & 4)) reply.Depth.assign(depth.begin(), depth.end());
		if (encoding & 32)
			reply.Normal = std::move(expandedAmbient[0]);
		else
			reply.Normal.assign(ambient[0].begin(), ambient[0].end());
		if (encoding & 64)
			reply.AmbientResponse = std::move(expandedAmbient[1]);
		else
			reply.AmbientResponse.assign(ambient[1].begin(), ambient[1].end());
		if (encoding & 128)
			reply.LightingBaseline = std::move(expandedAmbient[2]);
		else
			reply.LightingBaseline.assign(ambient[2].begin(), ambient[2].end());
		if (encoding & 512)
			reply.DirectionalResponse = std::move(expandedAmbient[3]);
		else
			reply.DirectionalResponse.assign(ambient[3].begin(), ambient[3].end());
		out = std::move(reply);
		error.clear();
		return true;
	}

	namespace {
		bool LayerExtent(uint32_t width, uint32_t height, size_t transparent, bool overlay = false) {
			return transparent <= MAX_PORTAL_TRANSPARENT_LAYERS && Extent(width, height) &&
				   size_t(width) * height <= MAX_PORTAL_IMAGE_PIXELS / (transparent + 1 + overlay);
		}
		bool ValidLayerSamples(const PortalImageReply &image) {
			if (!image.DirectionalResponse.empty() || !image.DirectionalResponseHash.IsZero()) return false;
			core::ByteReader depth(image.Depth);
			for (size_t offset = 0; offset < image.Pixels.size(); offset += 8) {
				const auto pixel = std::span(image.Pixels).subspan(offset, 8);
				const auto alpha =
					std::to_integer<uint16_t>(pixel[6]) | (std::to_integer<uint16_t>(pixel[7]) << 8);
				const float distance = depth.ReadFloat();
				if (alpha > 0x3c00 || (distance > 0 && alpha == 0)) return false;
				if (distance == 0)
					for (const auto byte : pixel)
						if (byte != std::byte{}) return false;
			}
			return true;
		}
		bool SameCapture(const PortalImageReply &image, const PortalImageReply &opaque, bool paired = true) {
			return image.Status == PortalImageStatus::Ok && image.Scope == PortalImageScope::OpaqueLighting &&
				   image.Key == opaque.Key && image.Width == opaque.Width && image.Height == opaque.Height &&
				   image.CaptureTick == opaque.CaptureTick &&
				   image.ContentRevision == opaque.ContentRevision &&
				   image.LightingRevision == opaque.LightingRevision &&
				   image.CaptureLighting == opaque.CaptureLighting &&
				   image.Depth.size() == (paired ? size_t(image.Width) * image.Height * 4 : 0);
		}
		bool ValidOverlaySamples(const PortalImageReply &image) {
			if (image.Pixels.size() % 8 != 0) return false;
			for (size_t offset = 0; offset < image.Pixels.size(); offset += 8) {
				const auto pixel = std::span(image.Pixels).subspan(offset, 8);
				const auto alpha =
					std::to_integer<uint16_t>(pixel[6]) | (std::to_integer<uint16_t>(pixel[7]) << 8);
				if (alpha > 0x3c00) return false;
				if (alpha == 0)
					for (const auto byte : pixel)
						if (byte != std::byte{}) return false;
			}
			return true;
		}
	}

	bool ValidPortalCaptureLenses(const PortalCaptureLenses &lenses) {
		if (lenses.Entries.size() > MAX_PORTAL_CAPTURE_LENSES || !std::isfinite(lenses.TimeSeconds) ||
			(lenses.Entries.empty() && (lenses.TimeSeconds != 0 || std::signbit(lenses.TimeSeconds))))
			return false;
		if (lenses.Programs.size() > MAX_PORTAL_CAPTURE_LENSES ||
			lenses.Programs.size() > lenses.Entries.size())
			return false;
		size_t codeBytes = 0;
		for (size_t index = 0; index < lenses.Programs.size(); ++index) {
			const auto &program = lenses.Programs[index];
			if (program.SpirV.size() > (MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES - codeBytes) / 4 ||
				!ValidLensProgram(program.Hash, std::as_bytes(std::span(program.SpirV))))
				return false;
			codeBytes += program.SpirV.size() * 4;
			for (size_t previous = 0; previous < index; ++previous)
				if (lenses.Programs[previous].Hash == program.Hash) return false;
			bool used = false;
			for (const auto &lens : lenses.Entries)
				used |= lens.ProgramHash == program.Hash;
			if (!used) return false;
		}
		for (const auto &lens : lenses.Entries) {
			if (!ValidCaptureLens(lens, lens.Shader)) return false;
			if (lenses.Programs.empty()) continue;
			bool covered = false;
			for (const auto &program : lenses.Programs)
				covered |= lens.ProgramHash == program.Hash;
			if (!covered) return false;
		}
		return true;
	}
	size_t PortalCaptureLensBytes(const PortalCaptureLenses &lenses) {
		size_t bytes = lenses.Entries.size() * sizeof(PortalCaptureLens);
		for (const auto &lens : lenses.Entries)
			bytes += lens.Shader.size();
		bytes += lenses.Programs.size() * sizeof(PortalCaptureLensProgram);
		for (const auto &program : lenses.Programs)
			bytes += program.SpirV.size() * sizeof(uint32_t);
		return bytes;
	}

	std::optional<PortalImageReplyMatch> MatchPortalImageLayerSet(
		std::span<const std::byte> bytes, const PortalExchangeKey &expected, uint32_t width, uint32_t height
	) {
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES || !ValidKey(expected) ||
			!LayerExtent(width, height, MAX_PORTAL_TRANSPARENT_LAYERS))
			return {};
		core::ByteReader reader(bytes);
		if (!Header(reader, 5)) return {};
		const auto request = reader.ReadUInt64();
		const auto portal = reader.ReadString();
		const auto camera = reader.ReadUInt64();
		const auto seam = reader.ReadUInt64();
		const auto imageWidth = reader.ReadUInt32();
		const auto imageHeight = reader.ReadUInt32();
		const auto count = reader.ReadUInt8();
		const auto overlay = reader.ReadUInt8();
		if (reader.Failed() || request != expected.RequestId || portal != expected.PortalKey ||
			camera != expected.CameraRevision || seam != expected.SeamRevision || imageWidth != width ||
			imageHeight != height || count != MAX_PORTAL_TRANSPARENT_LAYERS || overlay > 1 ||
			!LayerExtent(width, height, count, overlay != 0))
			return {};
		PortalImageReplyMatch result{PortalImageStatus::Ok};
		result.ImageCount = count + 1 + overlay;
		if (!CaptureLenses(reader, nullptr, result.MetadataBytes)) return {};
		for (size_t index = 0; index < result.ImageCount; ++index) {
			const auto length = reader.ReadUInt32();
			const auto member = reader.ReadRawView(length);
			const auto match =
				MatchPortalImageReply(member, expected, width, height, PortalImageScope::OpaqueLighting);
			if (reader.Failed() || !match || match->Status != PortalImageStatus::Ok ||
				(match->DepthBytes != 0) != (index <= count) ||
				(index != 0 && match->AmbientBytes > size_t(width) * height * 36))
				return {};
			if (index == 0)
				result.CaptureTick = match->CaptureTick;
			else if (result.CaptureTick != match->CaptureTick)
				return {};
			result.DepthBytes += match->DepthBytes;
			result.AmbientBytes += match->AmbientBytes;
			result.DiagnosticBytes += match->DiagnosticBytes;
		}
		return reader.AtEnd() ? std::optional(result) : std::nullopt;
	}

	struct LayerCapturePrefix {
		uint64_t Content = 0, Lighting = 0;
		std::optional<PortalCaptureLighting> Light;
		bool operator==(const LayerCapturePrefix &) const = default;
	};
	static bool PreflightLayerPayload(std::span<const std::byte> bytes, LayerCapturePrefix &prefix) {
		core::ByteReader r(bytes);
		if (!Header(r, 2)) return false;
		r.ReadUInt64();
		r.ReadString();
		r.ReadUInt64();
		r.ReadUInt64();
		r.ReadUInt8();
		r.ReadUInt8();
		const auto encoding = r.ReadUInt16();
		r.ReadString();
		r.ReadUInt64();
		prefix.Content = r.ReadUInt64();
		prefix.Lighting = r.ReadUInt64();
		const auto width = r.ReadUInt32(), height = r.ReadUInt32(), stride = r.ReadUInt32();
		if (!Extent(width, height) || stride != width * 8) return false;
		if (encoding & 8) CaptureLighting(r, prefix.Light.emplace());
		if (!ValidCaptureLighting(prefix.Light ? &*prefix.Light : nullptr)) return false;
		const auto payload = [&](size_t expanded, bool compressed) {
			r.ReadRawView(32);
			const auto length = r.ReadUInt32();
			const auto data = r.ReadRawView(length);
			if (r.Failed()) return false;
			if (!compressed) return length == expanded;
			return length < expanded && ZSTD_getFrameContentSize(data.data(), data.size()) == expanded &&
				   ZSTD_findFrameCompressedSize(data.data(), data.size()) == data.size();
		};
		if (!payload(size_t(width) * height * 8, encoding & 1)) return false;
		if ((encoding & 2) && !payload(size_t(width) * height * 4, encoding & 4)) return false;
		if (encoding & 16) {
			if (!payload(size_t(width) * height * 4, encoding & 32) ||
				!payload(size_t(width) * height * 16, encoding & 64) ||
				!payload(size_t(width) * height * 16, encoding & 128))
				return false;
		}
		if ((encoding & 256) && !payload(size_t(width) * height * 16, encoding & 512)) return false;
		return !r.Failed() && r.AtEnd();
	}
	bool PreflightPortalLayers(std::span<const std::byte> bytes, PortalLayerMeasure &out) {
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES) return false;
		core::ByteReader reader(bytes);
		if (!Header(reader, 5)) return false;
		struct KeyView {
			uint64_t RequestId;
			std::string_view PortalKey;
			uint64_t CameraRevision, SeamRevision;
		};
		const KeyView key{reader.ReadUInt64(), reader.ReadString(), reader.ReadUInt64(), reader.ReadUInt64()};
		PortalLayerMeasure result;
		result.RequestId = key.RequestId;
		result.PortalKey = key.PortalKey;
		result.CameraRevision = key.CameraRevision;
		result.SeamRevision = key.SeamRevision;
		result.Width = reader.ReadUInt32();
		result.Height = reader.ReadUInt32();
		const auto count = reader.ReadUInt8();
		const auto overlay = reader.ReadUInt8();
		if (reader.Failed() || overlay > 1 || !LayerExtent(result.Width, result.Height, count, overlay != 0))
			return false;
		result.Match.Status = PortalImageStatus::Ok;
		result.Match.ImageCount = size_t(count) + 1 + overlay;
		{
			auto programs = reader;
			programs.ReadUInt8();
			programs.ReadFloat();
			result.ProgramCount = programs.ReadUInt8();
			if (result.ProgramCount > MAX_PORTAL_CAPTURE_LENSES) return false;
			for (size_t i = 0; i < result.ProgramCount; ++i) {
				programs.ReadRaw(result.ProgramHashes[i].Digest.data(), 32);
				const auto words = programs.ReadUInt32();
				if (words > (MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES - result.ProgramBytes) / 4) return false;
				result.ProgramBytes += size_t(words) * 4;
				programs.ReadRawView(size_t(words) * 4);
			}
			if (programs.Failed()) return false;
		}
		if (!CaptureLenses(reader, nullptr, result.Match.MetadataBytes)) return false;
		result.Match.MetadataBytes += key.PortalKey.size() * result.Match.ImageCount;
		LayerCapturePrefix capture;
		for (size_t i = 0; i < result.Match.ImageCount; ++i) {
			const auto length = reader.ReadUInt32();
			const auto member = reader.ReadRawView(length);
			const auto match =
				MatchReply(member, key, result.Width, result.Height, PortalImageScope::OpaqueLighting);
			if (reader.Failed() || !match || match->Status != PortalImageStatus::Ok ||
				(match->DepthBytes != 0) != (i <= count) ||
				(i != 0 && match->AmbientBytes > size_t(result.Width) * result.Height * 36))
				return false;
			LayerCapturePrefix memberCapture;
			if (!PreflightLayerPayload(member, memberCapture) || !memberCapture.Light) return false;
			if (i == 0)
				capture = memberCapture;
			else if (capture != memberCapture)
				return false;
			if (i == 0)
				result.Match.CaptureTick = match->CaptureTick;
			else if (result.Match.CaptureTick != match->CaptureTick)
				return false;
			result.Match.DepthBytes += match->DepthBytes;
			result.Match.AmbientBytes += match->AmbientBytes;
			result.Match.DiagnosticBytes += match->DiagnosticBytes;
		}
		if (!reader.AtEnd()) return false;
		out = result;
		return true;
	}

	bool ValidPortalImageLayerSet(const PortalImageLayerSet &layers) {
		const auto &opaque = layers.Opaque;
		if (!ValidPortalCaptureLenses(layers.Lenses)) return false;
		if (!LayerExtent(
				opaque.Width, opaque.Height, layers.Transparent.size(), layers.SpatialOverlay.has_value()
			) ||
			!SameCapture(opaque, opaque) || !ValidReply(opaque, opaque.Pixels, opaque.Depth))
			return false;
		for (const auto &layer : layers.Transparent)
			if (!SameCapture(layer, opaque) || !ValidReply(layer, layer.Pixels, layer.Depth) ||
				!ValidLayerSamples(layer))
				return false;
		return !layers.SpatialOverlay ||
			   (SameCapture(*layers.SpatialOverlay, opaque, false) &&
				ValidReply(
					*layers.SpatialOverlay, layers.SpatialOverlay->Pixels, layers.SpatialOverlay->Depth
				) &&
				ValidOverlaySamples(*layers.SpatialOverlay));
	}

	bool EncodePortalImageLayerSet(
		const PortalImageLayerSet &layers, std::vector<std::byte> &out, std::string &error
	) {
		const auto &opaque = layers.Opaque;
		if (!ValidPortalCaptureLenses(layers.Lenses)) return Refuse(error, "invalid portal capture lenses");
		if (!ValidKey(opaque.Key) ||
			!LayerExtent(
				opaque.Width, opaque.Height, layers.Transparent.size(), layers.SpatialOverlay.has_value()
			) ||
			!SameCapture(opaque, opaque))
			return Refuse(error, "invalid portal layer layout");
		for (const auto &layer : layers.Transparent)
			if (!SameCapture(layer, opaque)) return Refuse(error, "portal layers do not share a capture");
		if (layers.SpatialOverlay && (!SameCapture(*layers.SpatialOverlay, opaque, false) ||
									  !ValidOverlaySamples(*layers.SpatialOverlay)))
			return Refuse(error, "invalid portal spatial overlay");
		core::ByteWriter writer;
		Header(writer, 5);
		Key(writer, opaque.Key);
		writer.WriteUInt32(opaque.Width);
		writer.WriteUInt32(opaque.Height);
		writer.WriteUInt8(static_cast<uint8_t>(layers.Transparent.size()));
		writer.WriteUInt8(layers.SpatialOverlay.has_value());
		CaptureLenses(writer, layers.Lenses);
		const size_t count = layers.Transparent.size() + 1 + layers.SpatialOverlay.has_value();
		std::array<std::vector<std::byte>, MAX_PORTAL_TRANSPARENT_LAYERS + 2> members;
		size_t wireBytes = writer.Size();
		for (size_t index = 0; index < count; ++index) {
			const auto &image = index == 0							 ? opaque
								: index <= layers.Transparent.size() ? layers.Transparent[index - 1]
																	 : *layers.SpatialOverlay;
			auto &member = members[index];
			if (!EncodePortalImageReply(image, member, error)) return false;
			if (index != 0 && index <= layers.Transparent.size() && !ValidLayerSamples(image))
				return Refuse(error, "invalid transparent layer samples");
			wireBytes += 4 + member.size();
			if (wireBytes > MAX_PORTAL_EXCHANGE_BYTES)
				return Refuse(error, "portal layer set exceeds wire budget");
		}
		// Reserve the actual compressed size, then copy each member once into the envelope.
		std::vector<std::byte> encoded;
		encoded.reserve(wireBytes);
		encoded.insert(encoded.end(), writer.Bytes().begin(), writer.Bytes().end());
		for (size_t index = 0; index < count; ++index) {
			writer.Clear();
			writer.WriteUInt32(static_cast<uint32_t>(members[index].size()));
			encoded.insert(encoded.end(), writer.Bytes().begin(), writer.Bytes().end());
			encoded.insert(encoded.end(), members[index].begin(), members[index].end());
		}
		out = std::move(encoded);
		error.clear();
		return true;
	}

	bool DecodePortalImageLayerSet(
		std::span<const std::byte> bytes, PortalImageLayerSet &out, std::string &error
	) {
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES)
			return Refuse(error, "portal layer set exceeds wire budget");
		core::ByteReader reader(bytes);
		PortalExchangeKey key;
		if (!Header(reader, 5) || !Key(reader, key)) return Refuse(error, "invalid portal layer header");
		const auto width = reader.ReadUInt32();
		const auto height = reader.ReadUInt32();
		const auto count = reader.ReadUInt8();
		const auto overlay = reader.ReadUInt8();
		if (reader.Failed() || overlay > 1 || !LayerExtent(width, height, count, overlay != 0))
			return Refuse(error, "invalid portal layer layout");
		PortalImageLayerSet layers;
		size_t lensBytes = 0;
		if (!CaptureLenses(reader, &layers.Lenses, lensBytes))
			return Refuse(error, "invalid portal capture lenses");
		// Admit all member layouts before decompressing even the first image.
		std::array<std::span<const std::byte>, MAX_PORTAL_TRANSPARENT_LAYERS + 2> members{};
		for (size_t index = 0; index <= size_t(count) + overlay; ++index) {
			const auto length = reader.ReadUInt32();
			members[index] = reader.ReadRawView(length);
			const auto match =
				MatchPortalImageReply(members[index], key, width, height, PortalImageScope::OpaqueLighting);
			if (reader.Failed() || !match || match->Status != PortalImageStatus::Ok ||
				(match->DepthBytes != 0) != (index <= count) ||
				(index != 0 && match->AmbientBytes > size_t(width) * height * 36))
				return Refuse(error, "invalid portal layer member");
		}
		if (!reader.AtEnd()) return Refuse(error, "trailing portal layer bytes");
		if (!DecodePortalImageReply(members[0], layers.Opaque, error)) return false;
		layers.Transparent.resize(count);
		for (size_t index = 0; index < count; ++index) {
			auto &layer = layers.Transparent[index];
			if (!DecodePortalImageReply(members[index + 1], layer, error)) return false;
			if (!SameCapture(layer, layers.Opaque))
				return Refuse(error, "portal layers do not share a capture");
			if (!ValidLayerSamples(layer)) return Refuse(error, "invalid transparent layer samples");
		}
		if (overlay) {
			layers.SpatialOverlay.emplace();
			if (!DecodePortalImageReply(members[count + 1], *layers.SpatialOverlay, error)) return false;
			if (!SameCapture(*layers.SpatialOverlay, layers.Opaque, false) ||
				!ValidOverlaySamples(*layers.SpatialOverlay))
				return Refuse(error, "invalid portal spatial overlay");
		}
		out = std::move(layers);
		error.clear();
		return true;
	}

}
