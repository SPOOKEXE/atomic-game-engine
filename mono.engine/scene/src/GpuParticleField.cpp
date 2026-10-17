#include <engine/core/Bytes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/GpuParticleField.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace engine::scene {
	namespace {
		bool GpuFieldMutationAllowed(const ecs::Store &store, ecs::Entity instance) {
			return !store.AdoptOnly() ||
				   (ecs::Store::IsPredicted(instance) && ecs::IsClientLocalInstance(store, instance));
		}
		constexpr size_t DEFINITION_BYTES =
			3 * 32 + 4 + MAX_GPU_PARTICLE_SPAWN_SAMPLES * GPU_PARTICLE_SPAWN_SAMPLE_BYTES;
		bool FieldFinite(float value, float limit) {
			return std::isfinite(value) && std::abs(value) <= limit;
		}
		bool FieldVectorValid(core::Vector3 value, float limit) {
			return FieldFinite(value.X, limit) && FieldFinite(value.Y, limit) && FieldFinite(value.Z, limit);
		}
		bool GpuFieldStyleValid(const GpuParticleStyle &style) {
			return FieldFinite(style.Colour.R, 1.0f) && style.Colour.R >= 0 &&
				   FieldFinite(style.Colour.G, 1.0f) && style.Colour.G >= 0 &&
				   FieldFinite(style.Colour.B, 1.0f) && style.Colour.B >= 0 &&
				   FieldFinite(style.Alpha, 1.0f) && style.Alpha >= 0 && FieldFinite(style.Size, 64.0f) &&
				   style.Size >= 0 && FieldVectorValid(style.Acceleration, 100000.0f);
		}
		void FieldWriteVector(core::ByteWriter &writer, core::Vector3 value) {
			writer.WriteFloat(value.X);
			writer.WriteFloat(value.Y);
			writer.WriteFloat(value.Z);
		}
		core::Vector3 FieldReadVector(core::ByteReader &reader) {
			return {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
		}
		void GpuFieldWriteSamples(core::ByteWriter &writer, std::span<const GpuParticleSpawnSample> samples) {
			for (const auto &sample : samples) {
				FieldWriteVector(writer, sample.Position);
				writer.WriteFloat(sample.Lifetime);
				FieldWriteVector(writer, sample.Velocity);
				writer.WriteUInt32(sample.Layer);
			}
		}
		bool GpuFieldReadSamples(std::span<const std::byte> bytes, std::vector<GpuParticleSpawnSample> &out) {
			if (bytes.size() % GPU_PARTICLE_SPAWN_SAMPLE_BYTES != 0 ||
				bytes.size() > MAX_GPU_PARTICLE_SPAWN_SAMPLES * GPU_PARTICLE_SPAWN_SAMPLE_BYTES)
				return false;
			core::ByteReader reader(bytes);
			std::vector<GpuParticleSpawnSample> candidate;
			candidate.reserve(bytes.size() / GPU_PARTICLE_SPAWN_SAMPLE_BYTES);
			while (reader.Remaining() != 0) {
				GpuParticleSpawnSample sample;
				sample.Position = FieldReadVector(reader);
				sample.Lifetime = reader.ReadFloat();
				sample.Velocity = FieldReadVector(reader);
				sample.Layer = reader.ReadUInt32();
				if (!FieldVectorValid(sample.Position, 1000000.0f) ||
					!FieldVectorValid(sample.Velocity, 100000.0f) || !FieldFinite(sample.Lifetime, 3600.0f) ||
					sample.Lifetime <= 0 || sample.Layer >= 3 || reader.Failed())
					return false;
				candidate.push_back(sample);
			}
			out = std::move(candidate);
			return true;
		}
	}
	uint32_t NormalizeGpuParticleCount(uint32_t requested) {
		constexpr std::array<uint32_t, 9> PRESETS{
			262144u, 524288u, 1048576u, 2000000u, 5000000u, 10000000u, 15000000u, 20000000u, 50000000u
		};
		if (requested == 0) return PRESETS[2];
		for (uint32_t preset : PRESETS)
			if (requested <= preset) return preset;
		return PRESETS.back();
	}
	bool ValidGpuParticleField(const GpuParticleField &field) {
		if ((field.Layers & ~GPU_PARTICLE_ALL_LAYERS) != 0 ||
			!FieldVectorValid(field.HalfExtent, 1000000.0f) || field.HalfExtent.X <= 0 ||
			field.HalfExtent.Y <= 0 || field.HalfExtent.Z <= 0 ||
			!FieldFinite(field.VelocityResponse, 1000.0f) || field.VelocityResponse < 0 ||
			field.SpawnSamples.size() > MAX_GPU_PARTICLE_SPAWN_SAMPLES)
			return false;
		for (const auto &style : field.Styles)
			if (!GpuFieldStyleValid(style)) return false;
		for (const auto &sample : field.SpawnSamples) {
			if (!FieldVectorValid(sample.Position, 1000000.0f) ||
				!FieldVectorValid(sample.Velocity, 100000.0f) || !FieldFinite(sample.Lifetime, 3600.0f) ||
				sample.Lifetime <= 0 || sample.Layer >= 3)
				return false;
		}
		return true;
	}
	bool HasGpuParticleLayer(const GpuParticleField &field, GpuParticleLayer layer) {
		return field.Enabled && (field.Layers & static_cast<uint8_t>(layer)) != 0;
	}
	bool
	SetGpuParticleSpawnSamples(ecs::Store &store, ecs::Entity instance, std::span<const std::byte> bytes) {
		if (!GpuFieldMutationAllowed(store, instance)) return false;
		const auto *field = store.Get<GpuParticleField>(instance);
		if (field == nullptr) return false;
		std::vector<GpuParticleSpawnSample> candidate;
		if (!GpuFieldReadSamples(bytes, candidate)) return false;
		if (field->SpawnSamples == candidate) return true;
		auto next = *field;
		next.SpawnSamples = std::move(candidate);
		store.Set(instance, std::move(next));
		return true;
	}
	bool SetGpuParticleStyle(
		ecs::Store &store, ecs::Entity instance, uint32_t index, const GpuParticleStyle &style
	) {
		if (!GpuFieldMutationAllowed(store, instance) || index >= 3 || !GpuFieldStyleValid(style))
			return false;
		const auto *field = store.Get<GpuParticleField>(instance);
		if (field == nullptr) return false;
		if (field->Styles[index] == style) return true;
		auto next = *field;
		next.Styles[index] = style;
		store.Set(instance, std::move(next));
		return true;
	}
	std::string GpuParticleDefinition(const GpuParticleField &field) {
		if (!ValidGpuParticleField(field)) return {};
		core::ByteWriter writer(0, DEFINITION_BYTES);
		for (const auto &style : field.Styles) {
			writer.WriteFloat(style.Colour.R);
			writer.WriteFloat(style.Colour.G);
			writer.WriteFloat(style.Colour.B);
			writer.WriteFloat(style.Alpha);
			writer.WriteFloat(style.Size);
			FieldWriteVector(writer, style.Acceleration);
		}
		writer.WriteUInt32(static_cast<uint32_t>(field.SpawnSamples.size()));
		GpuFieldWriteSamples(writer, field.SpawnSamples);
		constexpr char HEX[] = "0123456789abcdef";
		std::string result = "v1:";
		result.reserve(3 + writer.Bytes().size() * 2);
		for (std::byte byte : writer.Bytes()) {
			const auto value = std::to_integer<uint8_t>(byte);
			result += HEX[value >> 4];
			result += HEX[value & 15];
		}
		return result;
	}
	bool ReadGpuParticleDefinition(std::string_view text, GpuParticleField &field) {
		if (!text.starts_with("v1:") || text.size() > 3 + 2 * DEFINITION_BYTES || (text.size() - 3) % 2 != 0)
			return false;
		const auto digit = [](char ch) -> int {
			return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
		};
		std::vector<std::byte> bytes;
		bytes.reserve((text.size() - 3) / 2);
		for (size_t index = 3; index < text.size(); index += 2) {
			const int high = digit(text[index]), low = digit(text[index + 1]);
			if (high < 0 || low < 0) return false;
			bytes.push_back(static_cast<std::byte>((high << 4) | low));
		}
		core::ByteReader reader(bytes);
		auto candidate = field;
		for (auto &style : candidate.Styles) {
			style.Colour = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
			style.Alpha = reader.ReadFloat();
			style.Size = reader.ReadFloat();
			style.Acceleration = FieldReadVector(reader);
		}
		const uint32_t count = reader.ReadUInt32();
		if (reader.Failed() || count > MAX_GPU_PARTICLE_SPAWN_SAMPLES ||
			reader.Remaining() != count * GPU_PARTICLE_SPAWN_SAMPLE_BYTES)
			return false;
		if (!GpuFieldReadSamples(
				std::span<const std::byte>(bytes).last(reader.Remaining()), candidate.SpawnSamples
			) ||
			!ValidGpuParticleField(candidate))
			return false;
		field = std::move(candidate);
		return true;
	}
}
