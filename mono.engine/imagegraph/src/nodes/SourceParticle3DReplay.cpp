#include "SourceParticle3DReplay.hpp"

#include "../MeshPayload.hpp"
#include "../NodeExecutors.hpp"
#include "../ValuePayload.hpp"
#include "SourceMeshFlatten.hpp"

#include <bit>
#include <new>
#include <stdexcept>

namespace engine::imagegraph::detail {
	namespace source_particle3d_replay {
		constexpr std::string_view SCHEMA = "pc.3_d_particle/html5-state-v1";
		constexpr std::array<std::string_view, 13> FIELDS{
			"schema",
			"pool",
			"buffer_index",
			"spawn_index",
			"max_buffer_index",
			"frame",
			"path_class",
			"path_xy",
			"path_z",
			"path_weight",
			"curve_maps",
			"buffers",
			"base_mesh"
		};
		void Write(BufferValue &buffer, uint64_t bits, size_t bytes) {
			for (size_t i = 0; i < bytes; ++i)
				buffer.Bytes.push_back(uint8_t(bits >> (i * 8)));
		}
		uint64_t Read(const BufferValue &buffer, size_t &offset, size_t bytes) {
			uint64_t value = 0;
			for (size_t i = 0; i < bytes; ++i)
				value |= uint64_t(buffer.Bytes[offset++]) << (i * 8);
			return value;
		}
		const Value *Field(const StructValue &s, std::string_view key) {
			for (const auto &[name, value] : s.Data->Fields)
				if (name == key) return &value;
			return nullptr;
		}
		template <class T> const T *Get(const StructValue &s, std::string_view key) {
			const auto *v = Field(s, key);
			return v ? std::get_if<T>(v) : nullptr;
		}
		bool Fail(NodeContext &c) {
			return c.Fail(
				Status::InvalidValue, "Particle 3D replay receipt schema or payload is invalid", "mesh"
			);
		}
	}
	bool EncodeSourceParticle3DReceipt(
		NodeContext &c,
		const SourceParticle3DState &state,
		const MeshValue3D &base,
		StructValue &result,
		AllocationReservation &reservation
	) try {
		ENGINE_PROFILE("imagegraph.particle3d.encode");
		using namespace source_particle3d_replay;
		if (!ValidateSourceParticle3DState(c, state)) return false;
		uint64_t headerWork = 0;
		if (!SourceMeshFlattenWork(base, headerWork) ||
			headerWork > 64000000 / std::max<size_t>(c.ProcessorCount, 1))
			return c.Fail(Status::LimitExceeded, "Particle 3D replay base exceeds work bounds");
		const uint64_t baseBytes = MeshStorageBytes<false>(base);
		if (baseBytes > Limits::MaximumArrayBytes)
			return c.Fail(Status::LimitExceeded, "Particle 3D base mesh clone exceeds bounds");
		if (baseBytes * 2 > 64000000 / std::max<size_t>(c.ProcessorCount, 1))
			return c.Fail(
				Status::LimitExceeded, "Particle 3D base validation exceeds whole processor work bounds"
			);
		if (!ValidMeshPayload(base)) return Fail(c);
		const uint64_t buffersBytes = state.Buffers[0].size() * 2 * 36 * sizeof(float),
					   curvesBytes = 5 * 33 * sizeof(double);
		uint64_t bytes = sizeof(StructData) + FIELDS.size() * sizeof(std::pair<std::string, Value>) +
						 buffersBytes + curvesBytes + baseBytes +
						 std::max(SCHEMA.size(), std::string{}.capacity());
		for (auto key : FIELDS)
			bytes += std::max(key.size(), std::string{}.capacity());
		auto charge = c.ReserveWorkspace(bytes, "mesh");
		if (!charge) return false;
		StructValue candidate;
		auto &fields = candidate.Data.emplace().Fields;
		fields.reserve(FIELDS.size());
		fields.emplace_back("schema", std::string(SCHEMA));
		fields.emplace_back("pool", int64_t(state.Buffers[0].size()));
		fields.emplace_back("buffer_index", int64_t(state.BufferIndex));
		fields.emplace_back("spawn_index", int64_t(state.SpawnIndex));
		fields.emplace_back("max_buffer_index", int64_t(state.MaximumBufferIndex));
		fields.emplace_back("frame", state.Frame);
		fields.emplace_back("path_class", int64_t(state.PathTemporary.Class));
		fields.emplace_back("path_xy", state.PathTemporary.Position);
		fields.emplace_back("path_z", state.PathTemporary.Z ? Value{*state.PathTemporary.Z} : Value{false});
		fields.emplace_back("path_weight", state.PathTemporary.Weight);
		BufferValue curves;
		curves.Bytes.reserve(curvesBytes);
		for (const auto &map : state.CurveMaps)
			for (double value : map)
				Write(curves, std::bit_cast<uint64_t>(value), 8);
		fields.emplace_back("curve_maps", std::move(curves));
		BufferValue buffers;
		buffers.Bytes.reserve(buffersBytes);
		for (const auto &buffer : state.Buffers)
			for (const auto &slot : buffer) {
				for (float value : slot.Transform.Fields)
					Write(buffers, std::bit_cast<uint32_t>(value), 4);
				for (float value : std::bit_cast<std::array<float, 16>>(slot.Particle))
					Write(buffers, std::bit_cast<uint32_t>(value), 4);
				for (float value : slot.StartPosition)
					Write(buffers, std::bit_cast<uint32_t>(value), 4);
			}
		fields.emplace_back("buffers", std::move(buffers));
		fields.emplace_back("base_mesh", base);
		result = std::move(candidate);
		reservation = std::move(*charge);
		return true;
	} catch (const std::bad_alloc &) {
		return c.Fail(Status::LimitExceeded, "Particle 3D replay encoding allocation failed");
	} catch (const std::length_error &) {
		return c.Fail(Status::LimitExceeded, "Particle 3D replay encoding exceeds container bounds");
	}
	bool DecodeSourceParticle3DReceipt(
		NodeContext &c, const StructValue &receipt, SourceParticle3DReceipt &result
	) try {
		ENGINE_PROFILE("imagegraph.particle3d.decode");
		using namespace source_particle3d_replay;
		if (!receipt.Data || receipt.Data->Fields.size() != FIELDS.size()) return Fail(c);
		for (size_t i = 0; i < FIELDS.size(); ++i)
			if (receipt.Data->Fields[i].first != FIELDS[i]) return Fail(c);
		const auto *schema = Get<std::string>(receipt, "schema");
		const auto *pool = Get<int64_t>(receipt, "pool");
		const auto *index = Get<int64_t>(receipt, "buffer_index"),
				   *spawn = Get<int64_t>(receipt, "spawn_index"),
				   *maximum = Get<int64_t>(receipt, "max_buffer_index"),
				   *frame = Get<int64_t>(receipt, "frame"), *pathClass = Get<int64_t>(receipt, "path_class");
		const auto *xy = Get<Vector2>(receipt, "path_xy");
		const auto *weight = Get<double>(receipt, "path_weight");
		const auto *curves = Get<BufferValue>(receipt, "curve_maps"),
				   *buffers = Get<BufferValue>(receipt, "buffers");
		const auto *base = Get<MeshValue3D>(receipt, "base_mesh");
		if (!schema || *schema != SCHEMA || !pool || *pool < 1 || *pool > 1024 || !index || *index < 0 ||
			*index > 1 || !spawn || *spawn < 0 || !maximum || *maximum < 0 || *maximum >= *pool || !frame ||
			!pathClass || *pathClass < 0 || *pathClass > 1 || !xy || !weight || !curves || !buffers ||
			!base || curves->Bytes.size() != 5 * 33 * sizeof(double) ||
			buffers->Bytes.size() != size_t(*pool) * 2 * 36 * sizeof(float))
			return Fail(c);
		const auto *z = Get<double>(receipt, "path_z");
		const auto *absent = Get<bool>(receipt, "path_z");
		if (!z && (!absent || *absent)) return Fail(c);
		uint64_t headerWork = 0;
		if (!SourceMeshFlattenWork(*base, headerWork) ||
			headerWork > 64000000 / std::max<size_t>(c.ProcessorCount, 1))
			return c.Fail(Status::LimitExceeded, "Particle 3D replay base exceeds work bounds");
		const uint64_t baseBytes = MeshStorageBytes<false>(*base);
		if (baseBytes > Limits::MaximumArrayBytes)
			return c.Fail(Status::LimitExceeded, "Particle 3D replay base mesh exceeds clone bounds");
		// Decode word count is charged to the whole processor before allocating or reading binary words.
		const uint64_t work = uint64_t(*pool) * 2 * 36 + 165 + baseBytes * 2;
		if (work > 64000000 / std::max<size_t>(c.ProcessorCount, 1))
			return c.Fail(
				Status::LimitExceeded, "Particle 3D replay decode exceeds whole processor work bounds"
			);
		if (!ValidMeshPayload(*base)) return Fail(c);
		auto stateCharge = c.ReserveWorkspace(uint64_t(*pool) * 2 * sizeof(SourceParticle3DSlot)),
			 baseCharge = c.ReserveWorkspace(baseBytes);
		if (!stateCharge || !baseCharge) return false;
		SourceParticle3DReceipt candidate;
		candidate.State.Charge = std::move(*stateCharge);
		candidate.BaseCharge = std::move(*baseCharge);
		candidate.BaseMesh = *base;
		auto &s = candidate.State;
		s.Initialized = true;
		s.BufferIndex = size_t(*index);
		s.SpawnIndex = uint64_t(*spawn);
		s.MaximumBufferIndex = size_t(*maximum);
		s.Frame = *frame;
		s.PathTemporary = {
			SourcePathPointClass(*pathClass), *xy, z ? std::optional<double>{*z} : std::nullopt, *weight
		};
		size_t offset = 0;
		for (auto &map : s.CurveMaps)
			for (double &value : map)
				value = std::bit_cast<double>(Read(*curves, offset, 8));
		offset = 0;
		for (auto &buffer : s.Buffers) {
			buffer.resize(size_t(*pool));
			for (auto &slot : buffer) {
				for (float &value : slot.Transform.Fields)
					value = std::bit_cast<float>(uint32_t(Read(*buffers, offset, 4)));
				std::array<float, 16> particle;
				for (float &value : particle)
					value = std::bit_cast<float>(uint32_t(Read(*buffers, offset, 4)));
				slot.Particle = std::bit_cast<ParticleRecord3D>(particle);
				for (float &value : slot.StartPosition)
					value = std::bit_cast<float>(uint32_t(Read(*buffers, offset, 4)));
			}
		}
		if (!ValidateSourceParticle3DState(c, s)) return false;
		result = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		return c.Fail(Status::LimitExceeded, "Particle 3D replay decoding allocation failed");
	} catch (const std::length_error &) {
		return c.Fail(Status::LimitExceeded, "Particle 3D replay decoding exceeds container bounds");
	}
}
