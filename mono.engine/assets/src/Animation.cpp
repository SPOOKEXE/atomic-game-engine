#include <engine/assets/Animation.hpp>

#include <cmath>

namespace engine::assets {
	namespace {
		constexpr uint64_t KEY_BYTES = 8u * sizeof(float);

		bool Finite(const core::CFrame &frame) {
			const float rotationSquare =
				frame.QuaternionX * frame.QuaternionX + frame.QuaternionY * frame.QuaternionY +
				frame.QuaternionZ * frame.QuaternionZ + frame.QuaternionW * frame.QuaternionW;
			return std::isfinite(frame.Position.X) && std::isfinite(frame.Position.Y) &&
				   std::isfinite(frame.Position.Z) && std::isfinite(frame.QuaternionX) &&
				   std::isfinite(frame.QuaternionY) && std::isfinite(frame.QuaternionZ) &&
				   std::isfinite(frame.QuaternionW) && std::abs(rotationSquare - 1.0f) <= 0.01f;
		}

		void WriteFrame(core::ByteWriter &writer, const core::CFrame &frame) {
			writer.WriteFloat(frame.Position.X);
			writer.WriteFloat(frame.Position.Y);
			writer.WriteFloat(frame.Position.Z);
			writer.WriteFloat(frame.QuaternionX);
			writer.WriteFloat(frame.QuaternionY);
			writer.WriteFloat(frame.QuaternionZ);
			writer.WriteFloat(frame.QuaternionW);
		}

		core::CFrame ReadFrame(core::ByteReader &reader) {
			core::CFrame frame;
			frame.Position.X = reader.ReadFloat();
			frame.Position.Y = reader.ReadFloat();
			frame.Position.Z = reader.ReadFloat();
			frame.QuaternionX = reader.ReadFloat();
			frame.QuaternionY = reader.ReadFloat();
			frame.QuaternionZ = reader.ReadFloat();
			frame.QuaternionW = reader.ReadFloat();
			return frame;
		}
	}

	bool AnimationData::IsValid() const {
		if (!std::isfinite(Duration) || Duration <= 0.0f || Channels.empty() ||
			Channels.size() > Animation::MAXIMUM_CHANNELS) {
			return false;
		}

		uint64_t keyCount = 0;
		uint16_t previousJoint = 0;
		bool firstChannel = true;
		for (const AnimationChannel &channel : Channels) {
			if (channel.Joint >= Animation::MAXIMUM_JOINTS || channel.Keys.empty() ||
				channel.Keys.size() > Animation::MAXIMUM_KEYS_PER_CHANNEL ||
				(!firstChannel && channel.Joint <= previousJoint)) {
				return false;
			}
			firstChannel = false;
			previousJoint = channel.Joint;
			keyCount += channel.Keys.size();
			if (keyCount > Animation::MAXIMUM_KEYS) {
				return false;
			}

			float previousTime = -1.0f;
			for (const AnimationKeyframe &key : channel.Keys) {
				if (!std::isfinite(key.Time) || key.Time < 0.0f || key.Time > Duration ||
					key.Time <= previousTime || !Finite(key.Transform)) {
					return false;
				}
				previousTime = key.Time;
			}
		}
		return true;
	}

	bool Animation::Write(core::ByteWriter &writer, const AnimationData &data) {
		if (!data.IsValid()) {
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteFloat(data.Duration);
		writer.WriteUInt32(static_cast<uint32_t>(data.Channels.size()));
		for (const AnimationChannel &channel : data.Channels) {
			writer.WriteUInt16(channel.Joint);
			writer.WriteUInt32(static_cast<uint32_t>(channel.Keys.size()));
			for (const AnimationKeyframe &key : channel.Keys) {
				writer.WriteFloat(key.Time);
				WriteFrame(writer, key.Transform);
			}
		}
		return true;
	}

	bool Animation::Read(core::ByteReader &reader, AnimationData &out) {
		if (reader.ReadUInt32() != MAGIC || reader.ReadUInt16() != VERSION) {
			return false;
		}
		AnimationData parsed;
		parsed.Duration = reader.ReadFloat();
		const uint32_t channelCount = reader.ReadUInt32();
		if (reader.Failed() || channelCount == 0 || channelCount > MAXIMUM_CHANNELS) {
			return false;
		}

		parsed.Channels.reserve(channelCount);
		uint64_t totalKeys = 0;
		for (uint32_t index = 0; index < channelCount; index++) {
			AnimationChannel channel;
			channel.Joint = reader.ReadUInt16();
			const uint32_t keyCount = reader.ReadUInt32();
			totalKeys += keyCount;
			if (reader.Failed() || keyCount == 0 || keyCount > MAXIMUM_KEYS_PER_CHANNEL ||
				totalKeys > MAXIMUM_KEYS ||
				static_cast<uint64_t>(keyCount) * KEY_BYTES > reader.Remaining()) {
				return false;
			}
			channel.Keys.resize(keyCount);
			for (AnimationKeyframe &key : channel.Keys) {
				key.Time = reader.ReadFloat();
				key.Transform = ReadFrame(reader);
			}
			parsed.Channels.push_back(std::move(channel));
		}
		if (reader.Failed() || !parsed.IsValid()) {
			return false;
		}
		out = std::move(parsed);
		return true;
	}
}
