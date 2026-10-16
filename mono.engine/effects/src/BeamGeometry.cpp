#include "BeamGeometry.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Services.hpp>

#include <algorithm>
#include <cmath>

namespace engine::effects::beam_geometry {
	using core::CFrame;
	using core::Vector2;
	using core::Vector3;

	uint32_t PackRibbonRgba(const core::Color3 &colour, float alpha) {
		const auto channel = [](float value) {
			return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
		};
		return channel(colour.R) | (channel(colour.G) << 8) | (channel(colour.B) << 16) |
			   (channel(alpha) << 24);
	}

	// A cubic Bezier between two points with two tangents.
	//
	// **De Casteljau's form rather than the expanded polynomial**, which is
	// two more multiplies and is numerically better behaved at the endpoints -
	// and, more to the point here, is the form where the four control points
	// are visible in the code. A beam's shape is the thing an author is
	// looking at when they read this.
	Vector3 Bezier(const Vector3 &from, const Vector3 &out, const Vector3 &in, const Vector3 &to, float t) {
		const Vector3 a = from.Lerp(out, t);
		const Vector3 b = out.Lerp(in, t);
		const Vector3 c = in.Lerp(to, t);
		return a.Lerp(b, t).Lerp(b.Lerp(c, t), t);
	}

	// Both ends of a ribbon segment, given a centre, a direction along the
	// ribbon and a half-width.
	//
	// **The side vector is the cross of the ribbon's own direction with the
	// eye direction**, which is what makes a camera-facing ribbon face the
	// camera: the strip stays in the plane containing its length and turns
	// about that axis until its face is towards the viewer.
	//
	// A degenerate cross - the ribbon pointing straight at the eye - falls back
	// to world up rather than to zero, because a zero side vector collapses the
	// strip to a line and the segment disappears. A beam seen exactly end-on is
	// meant to be thin, not absent.
	Vector3 RibbonSideVector(const Vector3 &along, const Vector3 &toEye) {
		const Vector3 side = along.Cross(toEye);
		if (side.Magnitude() > 1e-5f) {
			return side.Unit();
		}
		const Vector3 fallback = along.Cross(Vector3{0.0f, 1.0f, 0.0f});
		return fallback.Magnitude() > 1e-5f ? fallback.Unit() : Vector3{1.0f, 0.0f, 0.0f};
	}

	const scene::UpdateCadence *BeamPolicy(const ecs::Store &store, ecs::Entity beam) {
		for (ecs::Entity walk = beam; walk != ecs::NULL_ENTITY; walk = store.ParentOf(walk)) {
			if (const auto *policy = store.Get<scene::UpdateCadence>(walk)) return policy;
		}
		return nullptr;
	}

	bool PlayerOwned(const ecs::Store &store, ecs::Entity subject) {
		for (ecs::Entity walk = subject; walk != ecs::NULL_ENTITY; walk = store.ParentOf(walk)) {
			if (scene::PlayerOf(store, walk) != ecs::NULL_ENTITY ||
				scene::NetworkOwnerOf(store, walk) != ecs::NULL_ENTITY)
				return true;
		}
		return scene::PlayerOwning(store, subject) != ecs::NULL_ENTITY;
	}

	bool BeamFramesMatch(const CFrame &left, const CFrame &right) {
		return left.Position == right.Position && left.QuaternionX == right.QuaternionX &&
			   left.QuaternionY == right.QuaternionY && left.QuaternionZ == right.QuaternionZ &&
			   left.QuaternionW == right.QuaternionW;
	}

	bool Finite(const Vector3 &point) {
		return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z);
	}

	bool Finite(const CFrame &frame) {
		return Finite(frame.Position) && std::isfinite(frame.QuaternionX) &&
			   std::isfinite(frame.QuaternionY) && std::isfinite(frame.QuaternionZ) &&
			   std::isfinite(frame.QuaternionW);
	}

	BeamCadenceCache *PrepareBeamCache(ecs::Store &store) {
		auto *cache = store.ResourceMutable<BeamCadenceCache>();
		if (cache == nullptr && store.CountMatching<Beam>() != 0 &&
			store.CountMatching<scene::UpdateCadence>() != 0) {
			store.SetResource(BeamCadenceCache{});
			cache = store.ResourceMutable<BeamCadenceCache>();
		}
		if (cache == nullptr) return nullptr;
		if (!store.Observed<Beam>()) store.Observe<Beam>();
		const size_t capacity = std::min(cache->MaximumBeams, BeamCadenceCache::MAXIMUM_BEAMS);
		if (cache->Beams.size() > capacity) cache->Beams.clear();
		std::erase_if(cache->Beams, [&](const auto &entry) {
			const auto *beam = store.Get<Beam>(entry.first);
			if (beam == nullptr || !beam->Enabled) return true;
			const auto *policy = BeamPolicy(store, entry.first);
			return policy == nullptr || !scene::ValidUpdateCadence(*policy);
		});
		const uint64_t version = store.ComponentChangeVersion<Beam>();
		if (version != cache->BeamWriteVersion) {
			// A cleared or reset observation window cannot identify individual missed writes.
			if (version < cache->BeamWriteVersion ||
				cache->BeamWriteVersion < store.ComponentClearedChangeVersion<Beam>()) {
				for (auto &held : cache->Beams)
					held.second.Dirty = true;
			} else {
				store.EachChanged<Beam>([&](ecs::Entity entity, const Beam &) {
					const auto found = cache->Beams.find(entity);
					if (found != cache->Beams.end()) found->second.Dirty = true;
				});
			}
			cache->BeamWriteVersion = version;
		}
		cache->Rebuilt = cache->Skipped = cache->CriticalRebuilds = 0;
		cache->CapacityRefused = cache->InvalidSources = 0;
		return cache;
	}
	void EmitBeamPair(
		std::vector<RibbonVertex> &out,
		const Vector3 &top,
		const Vector3 &bottom,
		float along,
		uint32_t colour
	) {
		out.push_back(RibbonVertex{top, Vector2{along, 0}, colour});
		out.push_back(RibbonVertex{bottom, Vector2{along, 1}, colour});
	}

	std::array<BeamGeometrySample, BEAM_SEGMENTS + 1>
	SampleBeam(const Beam &beam, const CFrame &start, const CFrame &finish) {
		std::array<BeamGeometrySample, BEAM_SEGMENTS + 1> samples;
		// The control points are pushed along each attachment's own axis,
		// which is what an attachment carries an orientation for. A beam with
		// no curve size is a straight line and still runs through the Bezier -
		// with the controls on the segment, the curve is the segment.
		const Vector3 control0 =
			start.Position + start.VectorToWorldSpace(Vector3{0.0f, 0.0f, -1.0f}) * beam.CurveSize0;
		const Vector3 control1 =
			finish.Position + finish.VectorToWorldSpace(Vector3{0.0f, 0.0f, -1.0f}) * beam.CurveSize1;

		// The whole length, so the texture repeats by metres rather than by
		// segment - a beam stretched to twice its length should show twice as
		// much texture, not the same texture twice as wide.
		const float span = (finish.Position - start.Position).Magnitude();
		const float repeats = beam.TextureLength > 0.0f ? span / beam.TextureLength : 1.0f;

		Vector3 previous = start.Position;
		for (uint32_t segment = 0; segment <= BEAM_SEGMENTS; segment++) {
			const float t = static_cast<float>(segment) / static_cast<float>(BEAM_SEGMENTS);
			const Vector3 centre = Bezier(start.Position, control0, control1, finish.Position, t);

			// The direction is towards the *next* point at the start and from
			// the previous one after that, so the first segment is not
			// degenerate. A zero-length direction is what the fallback in
			// `SideVector` catches.
			const Vector3 along = segment == 0 ? (Bezier(
													  start.Position,
													  control0,
													  control1,
													  finish.Position,
													  1.0f / static_cast<float>(BEAM_SEGMENTS)
												  ) -
												  centre)
											   : (centre - previous);
			previous = centre;

			const float width = beam.Width0 + (beam.Width1 - beam.Width0) * t;
			const uint32_t colour =
				PackRibbonRgba(beam.Colour.Evaluate(t), 1.0f - beam.Transparency.Evaluate(t));
			samples[segment] = {centre, along, width * 0.5f, t * repeats, colour};
		}
		return samples;
	}

	void AppendBeam(
		std::span<const BeamGeometrySample> samples,
		const Beam &beam,
		const Vector3 &fixedSide,
		const Vector3 &eye,
		float elapsed,
		std::vector<RibbonVertex> &out
	) {
		// Scroll and camera-facing width remain current even when centreline sampling is skipped.
		const float scroll = std::fmod(elapsed * beam.TextureSpeed, 1.0f);
		for (const auto &sample : samples) {
			const Vector3 side =
				beam.FaceCamera ? RibbonSideVector(sample.Along, (eye - sample.Centre).Unit()) : fixedSide;
			const Vector3 offset = side * sample.HalfWidth;
			EmitBeamPair(
				out, sample.Centre + offset, sample.Centre - offset, sample.Coordinate + scroll, sample.Colour
			);
		}
	}

	void BuildBeam(
		const ecs::Store &store,
		const Beam &beam,
		const Vector3 &eye,
		float elapsed,
		std::vector<RibbonVertex> &out
	) {
		const CFrame start = scene::ResolveAttachment(store, beam.Attachment0);
		const CFrame finish = scene::ResolveAttachment(store, beam.Attachment1);
		const auto samples = SampleBeam(beam, start, finish);
		AppendBeam(samples, beam, start.VectorToWorldSpace(Vector3{1, 0, 0}), eye, elapsed, out);
	}

	bool ValidBeamSource(const Beam &beam, const CFrame &start, const CFrame &finish) {
		if (!Finite(start) || !Finite(finish) || beam.Colour.Count > core::SEQUENCE_CAPACITY ||
			beam.Transparency.Count > core::SEQUENCE_CAPACITY)
			return false;
		for (float scalar :
			 {beam.CurveSize0,
			  beam.CurveSize1,
			  beam.Width0,
			  beam.Width1,
			  beam.TextureSpeed,
			  beam.TextureLength}) {
			if (!std::isfinite(scalar)) return false;
		}
		for (uint32_t index = 0; index < beam.Colour.Count; ++index) {
			const auto &key = beam.Colour.Keypoints[index];
			if (!std::isfinite(key.Time) || !std::isfinite(key.Value.R) || !std::isfinite(key.Value.G) ||
				!std::isfinite(key.Value.B))
				return false;
		}
		for (uint32_t index = 0; index < beam.Transparency.Count; ++index) {
			const auto &key = beam.Transparency.Keypoints[index];
			if (!std::isfinite(key.Time) || !std::isfinite(key.Value) || !std::isfinite(key.Envelope))
				return false;
		}
		return true;
	}

	bool AppendCadencedBeam(
		ecs::Store &store,
		BeamCadenceCache &cache,
		ecs::Entity entity,
		const Beam &beam,
		const scene::UpdateCadence &policy,
		const Vector3 &eye,
		float elapsed,
		std::vector<RibbonVertex> &out
	) {
		const CFrame start = scene::ResolveAttachment(store, beam.Attachment0);
		const CFrame finish = scene::ResolveAttachment(store, beam.Attachment1);
		if (store.Get<scene::Attachment>(beam.Attachment0) == nullptr ||
			store.Get<scene::Attachment>(beam.Attachment1) == nullptr ||
			!ValidBeamSource(beam, start, finish) || !Finite(eye) || !std::isfinite(elapsed)) {
			cache.InvalidSources++;
			cache.Beams.erase(entity);
			return false;
		}
		uint32_t critical = 0;
		if (PlayerOwned(store, entity) || PlayerOwned(store, beam.Attachment0) ||
			PlayerOwned(store, beam.Attachment1) ||
			(policy.Owner != ecs::NULL_ENTITY && PlayerOwned(store, policy.Owner)))
			critical |= static_cast<uint32_t>(scene::UpdateCriticality::PlayerOwned);
		auto found = cache.Beams.find(entity);
		if (found == cache.Beams.end()) {
			if (cache.Beams.size() >= std::min(cache.MaximumBeams, BeamCadenceCache::MAXIMUM_BEAMS)) {
				cache.CapacityRefused++;
				BuildBeam(store, beam, eye, elapsed, out);
				cache.Rebuilt++;
				if ((critical | policy.CriticalReasons) != 0) cache.CriticalRebuilds++;
				return true;
			}
			found = cache.Beams.try_emplace(entity).first;
		}
		auto &entry = found->second;
		if (entry.Dirty || !BeamFramesMatch(start, entry.Start) || !BeamFramesMatch(finish, entry.Finish)) {
			entry.SourceRevision = entry.SourceRevision == UINT64_MAX ? 1 : entry.SourceRevision + 1;
		}
		const auto due = scene::CadenceDue(
			policy,
			entry.State,
			scene::UpdateDomain::CosmeticEffects,
			store.Time().Tick,
			entry.SourceRevision,
			critical
		);
		const bool fullFidelity =
			due.CriticalReasons != 0 || !policy.Identity.IsValid() ||
			policy.Domains[static_cast<size_t>(scene::UpdateDomain::CosmeticEffects)].IntervalTicks == 1;
		if (due.Due || fullFidelity) {
			const auto samples = SampleBeam(beam, start, finish);
			for (const auto &sample : samples) {
				if (!Finite(sample.Centre) || !Finite(sample.Along) || !std::isfinite(sample.HalfWidth) ||
					!std::isfinite(sample.Coordinate)) {
					cache.InvalidSources++;
					return false;
				}
			}
			auto committed = entry.State;
			if (due.Due && !scene::CommitUpdate(
							   committed, due, store.Time().Tick, entry.SourceRevision, policy.Revision, true
						   )) {
				cache.InvalidSources++;
				return false;
			}
			entry.State = committed;
			entry.Samples = samples;
			entry.Start = start;
			entry.Finish = finish;
			entry.FixedSide = start.VectorToWorldSpace(Vector3{1, 0, 0});
			entry.Dirty = false;
			cache.Rebuilt++;
			if (due.CriticalReasons != 0) cache.CriticalRebuilds++;
		} else {
			cache.Skipped++;
		}
		AppendBeam(entry.Samples, beam, entry.FixedSide, eye, elapsed, out);
		return true;
	}

	bool AppendBeamGeometry(
		ecs::Store &store,
		BeamCadenceCache *cache,
		ecs::Entity entity,
		const Beam &beam,
		const Vector3 &eye,
		float elapsed,
		std::vector<RibbonVertex> &output
	) {
		const auto *policy = BeamPolicy(store, entity);
		if (cache != nullptr && policy != nullptr && scene::ValidUpdateCadence(*policy))
			return AppendCadencedBeam(store, *cache, entity, beam, *policy, eye, elapsed, output);
		BuildBeam(store, beam, eye, elapsed, output);
		if (cache != nullptr) cache->Rebuilt++;
		return true;
	}
}
