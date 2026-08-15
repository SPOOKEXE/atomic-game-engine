#include <engine/core/Bytes.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Wire.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::scene {

	namespace {
		// Codes per unit, for each grid.
		//
		// **Every one of these is a whole number over a power of two, so it is
		// exact in a float and so is the division that undoes it.** That is the
		// property the decode side needs: both machines have to land on the
		// same value from the same code, and one correctly-rounded division by
		// a constant does, where a chain of arithmetic would only nearly.
		constexpr float POSITION_SCALE = static_cast<float>(WIRE_STEPS) / WIRE_POSITION_HALF_EXTENT_METRES;
		constexpr float LINEAR_SCALE =
			static_cast<float>(WIRE_STEPS) / WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND;
		constexpr float ANGULAR_SCALE =
			static_cast<float>(WIRE_STEPS) / WIRE_ANGULAR_HALF_EXTENT_RADIANS_PER_SECOND;
		constexpr float ROTATION_SCALE = static_cast<float>(WIRE_ROTATION_STEPS) / WIRE_ROTATION_LIMIT;

		// Ten bits per sent quaternion component, and the bias that makes the
		// signed range fit an unsigned field.
		constexpr uint32_t ROTATION_BITS = 10;
		constexpr uint32_t ROTATION_MASK = (1u << ROTATION_BITS) - 1u;

		// The wire form is fixed width, so the header's byte counts are a
		// promise `Authority` sizes messages against rather than a description.
		static_assert(WIRE_TRANSFORM_BYTES == 3 * sizeof(int16_t) + sizeof(uint32_t));
		static_assert(WIRE_MOTION_BYTES == 6 * sizeof(int16_t));

		// One axis, onto the grid.
		//
		// **Clamped rather than wrapped**, for the reason `Wire.hpp` gives: an
		// entity outside the stated extent piles up against a wall somebody can
		// see, where a wrapped one appears at the far side of the world and is
		// indistinguishable from a teleport the server meant.
		//
		// A non-finite input returns the origin. It is a bug upstream either
		// way, but `std::lround` of a NaN produces an unspecified value and a
		// wild code is a worse way to find out than a value at zero.
		//
		// **The scaling is done in double, and that is worth a line.** A
		// coordinate near the far edge times the scale is a number around
		// 32768, where a float's spacing is a quarter of a code - so a float
		// multiply here rounds to the wrong code often enough to push the worst
		// case measurably past half a step, and the stated bound would have
		// needed an apology in it. The decode stays in float, because that is
		// the half both machines have to agree on and a single division is
		// correctly rounded.
		int16_t Encode(float value, float scale) {
			if (!std::isfinite(value)) {
				return 0;
			}

			const auto limit = static_cast<double>(WIRE_STEPS);
			const double scaled = static_cast<double>(value) * static_cast<double>(scale);
			return static_cast<int16_t>(std::lround(std::clamp(scaled, -limit, limit)));
		}

		// One axis, back off it.
		//
		// **The code is clamped here too, and that is not belt and braces.**
		// The encoder never emits -32768, so a decode that trusted its input
		// would put a hostile peer's entity a step outside the extent this
		// module states everything is inside.
		float Decode(int16_t code, float scale) {
			return static_cast<float>(std::clamp<int32_t>(code, -WIRE_STEPS, WIRE_STEPS)) / scale;
		}

		// A unit quaternion as smallest-three: which component was dropped, and
		// the other three.
		//
		// The largest is dropped because it is the one whose recovery from unit
		// length is best conditioned - it is at least a half, so the division
		// that recovers it cannot amplify the other three's error. Ties go to
		// the lowest index, which is arbitrary and, more importantly, the same
		// arbitrary answer every time: two runs of one server must produce the
		// same bytes.
		uint32_t PackRotation(const core::CFrame &frame) {
			const float components[4] = {
				frame.QuaternionX, frame.QuaternionY, frame.QuaternionZ, frame.QuaternionW
			};

			uint32_t largest = 0;
			for (uint32_t index = 1; index < 4; index++) {
				if (std::abs(components[index]) > std::abs(components[largest])) {
					largest = index;
				}
			}

			// **q and -q are the same rotation**, so the whole quaternion is
			// negated when the dropped component is negative. That is what lets
			// the dropped one be recovered as a positive square root and its
			// sign never be sent - the bit the two-bit index is paid for with.
			const float sign = components[largest] < 0.0f ? -1.0f : 1.0f;

			uint32_t packed = largest << (32u - 2u);
			uint32_t shift = 0;
			for (uint32_t index = 0; index < 4; index++) {
				if (index == largest) {
					continue;
				}

				const auto limit = static_cast<double>(WIRE_ROTATION_STEPS);
				const double scaled =
					static_cast<double>(components[index] * sign) * static_cast<double>(ROTATION_SCALE);
				const auto code = static_cast<int32_t>(std::lround(std::clamp(scaled, -limit, limit)));
				packed |= (static_cast<uint32_t>(code + WIRE_ROTATION_STEPS) & ROTATION_MASK) << shift;
				shift += ROTATION_BITS;
			}
			return packed;
		}

		// The rotation back out, as a unit quaternion for every one of the four
		// billion words that could arrive.
		//
		// The normalise at the end is not tidying. `1 - s` is clamped at zero,
		// so a word no encoder would have produced - three components that do
		// not fit inside one rotation - still decodes to something, and without
		// the normalise that something is a quaternion of length up to 1.23
		// that every consumer downstream would treat as a rotation. The length
		// is provably at least one, so there is no zero to guard.
		void UnpackRotation(uint32_t packed, core::CFrame &frame) {
			const uint32_t largest = packed >> (32u - 2u);

			float components[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			float sum = 0.0f;
			uint32_t shift = 0;
			for (uint32_t index = 0; index < 4; index++) {
				if (index == largest) {
					continue;
				}

				const auto biased = static_cast<int32_t>((packed >> shift) & ROTATION_MASK);
				const int32_t code =
					std::clamp(biased - WIRE_ROTATION_STEPS, -WIRE_ROTATION_STEPS, WIRE_ROTATION_STEPS);
				shift += ROTATION_BITS;

				components[index] = static_cast<float>(code) / ROTATION_SCALE;
				sum += components[index] * components[index];
			}

			components[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));

			const float length = std::sqrt(
				components[0] * components[0] + components[1] * components[1] +
				components[2] * components[2] + components[3] * components[3]
			);

			frame.QuaternionX = components[0] / length;
			frame.QuaternionY = components[1] / length;
			frame.QuaternionZ = components[2] / length;
			frame.QuaternionW = components[3] / length;
		}

		void WriteTransforms(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *transforms = static_cast<const Transform *>(source);
			for (size_t index = 0; index < count; index++) {
				const core::CFrame &frame = transforms[index].Frame;
				writer.WriteInt16(Encode(frame.Position.X, POSITION_SCALE));
				writer.WriteInt16(Encode(frame.Position.Y, POSITION_SCALE));
				writer.WriteInt16(Encode(frame.Position.Z, POSITION_SCALE));
				writer.WriteUInt32(PackRotation(frame));
			}
		}

		void ReadTransforms(core::ByteReader &reader, void *destination, size_t count) {
			auto *transforms = static_cast<Transform *>(destination);
			for (size_t index = 0; index < count; index++) {
				core::CFrame &frame = transforms[index].Frame;
				frame.Position.X = Decode(reader.ReadInt16(), POSITION_SCALE);
				frame.Position.Y = Decode(reader.ReadInt16(), POSITION_SCALE);
				frame.Position.Z = Decode(reader.ReadInt16(), POSITION_SCALE);
				UnpackRotation(reader.ReadUInt32(), frame);
			}
		}

		void WriteMotions(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *motions = static_cast<const Motion *>(source);
			for (size_t index = 0; index < count; index++) {
				const Motion &motion = motions[index];
				writer.WriteInt16(Encode(motion.Linear.X, LINEAR_SCALE));
				writer.WriteInt16(Encode(motion.Linear.Y, LINEAR_SCALE));
				writer.WriteInt16(Encode(motion.Linear.Z, LINEAR_SCALE));
				writer.WriteInt16(Encode(motion.Angular.X, ANGULAR_SCALE));
				writer.WriteInt16(Encode(motion.Angular.Y, ANGULAR_SCALE));
				writer.WriteInt16(Encode(motion.Angular.Z, ANGULAR_SCALE));
			}
		}

		void ReadMotions(core::ByteReader &reader, void *destination, size_t count) {
			auto *motions = static_cast<Motion *>(destination);
			for (size_t index = 0; index < count; index++) {
				Motion &motion = motions[index];
				motion.Linear.X = Decode(reader.ReadInt16(), LINEAR_SCALE);
				motion.Linear.Y = Decode(reader.ReadInt16(), LINEAR_SCALE);
				motion.Linear.Z = Decode(reader.ReadInt16(), LINEAR_SCALE);
				motion.Angular.X = Decode(reader.ReadInt16(), ANGULAR_SCALE);
				motion.Angular.Y = Decode(reader.ReadInt16(), ANGULAR_SCALE);
				motion.Angular.Z = Decode(reader.ReadInt16(), ANGULAR_SCALE);
			}
		}
	}

	ecs::WireFormat TransformWire() {
		return ecs::WireFormat{WriteTransforms, ReadTransforms, WIRE_TRANSFORM_BYTES};
	}

	ecs::WireFormat MotionWire() {
		return ecs::WireFormat{WriteMotions, ReadMotions, WIRE_MOTION_BYTES};
	}
}
