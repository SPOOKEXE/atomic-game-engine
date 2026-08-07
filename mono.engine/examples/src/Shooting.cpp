#include <engine/examples/Shooting.hpp>

#include <cmath>

namespace engine::examples {

	namespace {
		// Allows normal floating-point direction error without accepting zero.
		constexpr float UNIT_TOLERANCE = 1e-3f;

		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}
	}

	std::vector<std::byte> EncodeShot(const Shot &shot) {
		core::ByteWriter writer;
		writer.WriteFloat(shot.Aim.Origin.X);
		writer.WriteFloat(shot.Aim.Origin.Y);
		writer.WriteFloat(shot.Aim.Origin.Z);
		writer.WriteFloat(shot.Aim.Direction.X);
		writer.WriteFloat(shot.Aim.Direction.Y);
		writer.WriteFloat(shot.Aim.Direction.Z);
		writer.WriteFloat(shot.Range);

		const std::span<const std::byte> bytes = writer.Bytes();
		return {bytes.begin(), bytes.end()};
	}

	bool DecodeShot(std::span<const std::byte> bytes, Shot &out) {
		core::ByteReader reader(bytes);

		Shot shot;
		shot.Aim.Origin.X = reader.ReadFloat();
		shot.Aim.Origin.Y = reader.ReadFloat();
		shot.Aim.Origin.Z = reader.ReadFloat();
		shot.Aim.Direction.X = reader.ReadFloat();
		shot.Aim.Direction.Y = reader.ReadFloat();
		shot.Aim.Direction.Z = reader.ReadFloat();
		shot.Range = reader.ReadFloat();

		if (reader.Failed() || reader.Remaining() != 0) {
			// The payload must contain exactly one shot.
			return false;
		}
		if (!Finite(shot.Aim.Origin) || !Finite(shot.Aim.Direction) || !std::isfinite(shot.Range)) {
			return false;
		}
		if (shot.Range <= 0.0f || shot.Range > MAXIMUM_SHOT_RANGE) {
			return false;
		}

		// Do not normalize an invalid client direction.
		const float length = std::sqrt(
			shot.Aim.Direction.X * shot.Aim.Direction.X + shot.Aim.Direction.Y * shot.Aim.Direction.Y +
			shot.Aim.Direction.Z * shot.Aim.Direction.Z
		);
		if (std::abs(length - 1.0f) > UNIT_TOLERANCE) {
			return false;
		}

		out = shot;
		return true;
	}

	Hit NearestHit(const Shot &shot, std::span<const Target> targets) {
		Hit best;

		for (const Target &target : targets) {
			if (target.Radius <= 0.0f || !Finite(target.At)) {
				continue;
			}

			// Solve the ray/sphere intersection along the ray.
			const core::Vector3 toCentre = target.At - shot.Aim.Origin;
			const float along = toCentre.X * shot.Aim.Direction.X + toCentre.Y * shot.Aim.Direction.Y +
								toCentre.Z * shot.Aim.Direction.Z;

			const float centreSquared =
				toCentre.X * toCentre.X + toCentre.Y * toCentre.Y + toCentre.Z * toCentre.Z;
			const float perpendicularSquared = centreSquared - along * along;
			const float radiusSquared = target.Radius * target.Radius;

			if (perpendicularSquared > radiusSquared) {
				continue;
			}

			// A shooter inside a sphere is hit at distance zero.
			const float half = std::sqrt(std::max(0.0f, radiusSquared - perpendicularSquared));
			const float entry = along - half;
			const float distance = entry < 0.0f ? 0.0f : entry;

			// Reject spheres entirely behind the ray origin.
			if (along + half < 0.0f) {
				continue;
			}
			if (distance > shot.Range) {
				continue;
			}

			// Resolve nearest distance, then entity id for deterministic ties.
			if (!best.Struck || distance < best.Distance ||
				(distance == best.Distance && target.Entity.Id < best.Entity.Id)) {
				best.Entity = target.Entity;
				best.Distance = distance;
				best.Struck = true;
			}
		}

		return best;
	}
}
