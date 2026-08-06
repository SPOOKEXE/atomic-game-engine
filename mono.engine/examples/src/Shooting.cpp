#include <engine/examples/Shooting.hpp>

#include <cmath>

namespace engine::examples {

	namespace {
		// How far a direction's length may sit from one and still be called
		// unit.
		//
		// A client computes this from a camera matrix in single precision, so
		// demanding exactness would refuse every honest shot. A thousandth is
		// far tighter than any accumulated error and far looser than the
		// zero-length direction the check exists to catch.
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
			// Trailing bytes mean the sender is not speaking this encoding, and
			// there is nothing to be gained by acting on the part that parsed.
			return false;
		}
		if (!Finite(shot.Aim.Origin) || !Finite(shot.Aim.Direction) || !std::isfinite(shot.Range)) {
			return false;
		}
		if (shot.Range <= 0.0f || shot.Range > MAXIMUM_SHOT_RANGE) {
			return false;
		}

		// **A direction of zero would put every point on the ray at the
		// origin**, so every target standing there is hit by every shot — and
		// the arithmetic below would report it as a perfectly ordinary hit at
		// distance zero. Refused rather than normalised: a client that sent one
		// is not aiming at anything, and inventing a direction for it would be
		// the server deciding where it shot.
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

			// Ray against sphere, solved along the ray rather than by squaring
			// a distance: `along` is how far the sphere's centre projects onto
			// the direction, and what is left is the perpendicular gap.
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

			// Where the ray enters the sphere. A shooter already inside one is
			// hit at distance zero rather than at a negative distance, which is
			// behind them.
			const float half = std::sqrt(std::max(0.0f, radiusSquared - perpendicularSquared));
			const float entry = along - half;
			const float distance = entry < 0.0f ? 0.0f : entry;

			// Entirely behind the shooter: the far intersection is negative
			// too, so the sphere is not on the ray at all.
			if (along + half < 0.0f) {
				continue;
			}
			if (distance > shot.Range) {
				continue;
			}

			// **Nearest, and ties broken by entity id.** The candidates arrive
			// in whatever order a hash map walked them, so "the first one that
			// intersects" is a different answer on two machines with the same
			// input — and a server whose hit resolution depends on map ordering
			// is one whose recordings do not replay.
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
