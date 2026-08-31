#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::effects {

	namespace {
		using core::CFrame;
		using core::Vector2;
		using core::Vector3;

		uint32_t PackRgba(const core::Color3 &colour, float alpha) {
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
		Vector3
		Bezier(const Vector3 &from, const Vector3 &out, const Vector3 &in, const Vector3 &to, float t) {
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
		Vector3 SideVector(const Vector3 &along, const Vector3 &toEye) {
			const Vector3 side = along.Cross(toEye);
			if (side.Magnitude() > 1e-5f) {
				return side.Unit();
			}
			const Vector3 fallback = along.Cross(Vector3{0.0f, 1.0f, 0.0f});
			return fallback.Magnitude() > 1e-5f ? fallback.Unit() : Vector3{1.0f, 0.0f, 0.0f};
		}
	}

	// --- trails ---------------------------------------------------------------

	size_t RecordTrails(ecs::Store &store, float delta) {
		ENGINE_PROFILE_CAT("record trails", core::ProfileCategory::Simulation);

		size_t recorded = 0;
		store.Each<Trail>([&store, delta, &recorded](ecs::Entity, Trail &trail) {
			// Ageing happens whether or not the trail is enabled, so a disabled
			// trail fades out rather than freezing. That is the other half of
			// "disabling does not clear": it stops recording and lets what is
			// there expire.
			for (uint32_t index = 0; index < trail.Recorded; index++) {
				trail.Age[(trail.Head + index) % TRAIL_POINTS] += delta;
			}

			// Retire from the tail, which is where the oldest is. One per step is
			// enough at any frame rate that matters - sixteen points over a
			// lifetime means a point expires every lifetime/16 seconds, and a step
			// long enough to expire two is a step in which the trail has already
			// stopped being continuous.
			while (trail.Recorded > 0) {
				const uint32_t tail = (trail.Head + trail.Recorded - 1) % TRAIL_POINTS;
				if (trail.Age[tail] < trail.Lifetime) {
					break;
				}
				trail.Recorded--;
			}

			if (!trail.Enabled) {
				return;
			}

			const CFrame top = scene::ResolveAttachment(store, trail.Attachment0);
			const CFrame bottom = scene::ResolveAttachment(store, trail.Attachment1);

			// **A new point every step, not only when the trail has moved.** A
			// stationary trail recording nothing looks correct until it starts
			// moving again, at which point the newest point is however old the
			// pause was and the first segment stretches across it. Recording
			// always keeps the ring's ages honest, and the minimum-angle test
			// below is what removes the segments that are not worth drawing.
			trail.Head = (trail.Head + TRAIL_POINTS - 1) % TRAIL_POINTS;
			trail.Top[trail.Head] = top.Position;
			trail.Bottom[trail.Head] = bottom.Position;
			trail.Age[trail.Head] = 0.0f;
			trail.Recorded = std::min(trail.Recorded + 1, TRAIL_POINTS);
			recorded++;
		});

		return recorded;
	}

	// --- building -------------------------------------------------------------

	namespace {
		// Appends one pair of vertices - one segment's worth of strip.
		void Emit(
			std::vector<RibbonVertex> &out,
			const Vector3 &top,
			const Vector3 &bottom,
			float along,
			uint32_t colour
		) {
			out.push_back(RibbonVertex{top, Vector2{along, 0.0f}, colour});
			out.push_back(RibbonVertex{bottom, Vector2{along, 1.0f}, colour});
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

			// The control points are pushed along each attachment's own axis,
			// which is what an attachment carries an orientation for. A beam with
			// no curve size is a straight line and still runs through the Bezier -
			// with the controls on the segment, the curve is the segment.
			const Vector3 control0 =
				start.Position + start.VectorToWorldSpace(Vector3{0.0f, 0.0f, -1.0f}) * beam.CurveSize0;
			const Vector3 control1 =
				finish.Position + finish.VectorToWorldSpace(Vector3{0.0f, 0.0f, -1.0f}) * beam.CurveSize1;

			// How far the texture has scrolled. Wrapped into `[0, 1)` here rather
			// than left to grow, because at ten minutes of elapsed time a float's
			// resolution over a scrolling coordinate is visibly steppy.
			const float scroll = std::fmod(elapsed * beam.TextureSpeed, 1.0f);

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

				const Vector3 side = beam.FaceCamera ? SideVector(along, (eye - centre).Unit())
													 : start.VectorToWorldSpace(Vector3{1.0f, 0.0f, 0.0f});

				const float width = beam.Width0 + (beam.Width1 - beam.Width0) * t;
				const Vector3 offset = side * (width * 0.5f);

				const uint32_t colour =
					PackRgba(beam.Colour.Evaluate(t), 1.0f - beam.Transparency.Evaluate(t));
				Emit(out, centre + offset, centre - offset, t * repeats + scroll, colour);
			}
		}

		void BuildTrail(const Trail &trail, const Vector3 &eye, std::vector<RibbonVertex> &out) {
			if (trail.Recorded < 2) {
				// One point is not a strip. Skipped rather than emitted as a
				// degenerate quad, which would draw a zero-area sliver that some
				// rasterisers show as a stray pixel.
				return;
			}

			const float cosLimit =
				std::cos((180.0f - trail.MinimumAngle) * std::numbers::pi_v<float> / 180.0f);

			Vector3 previousAlong;
			bool havePrevious = false;
			float travelled = 0.0f;

			for (uint32_t index = 0; index < trail.Recorded; index++) {
				const uint32_t at = (trail.Head + index) % TRAIL_POINTS;
				const Vector3 top = trail.Top[at];
				const Vector3 bottom = trail.Bottom[at];
				const Vector3 centre = (top + bottom) * 0.5f;

				if (index + 1 < trail.Recorded) {
					const uint32_t next = (trail.Head + index + 1) % TRAIL_POINTS;
					const Vector3 along = ((trail.Top[next] + trail.Bottom[next]) * 0.5f - centre);

					// **The doubling-back test.** Two consecutive segments turning
					// through more than `MinimumAngle` from a straight line
					// produce a bow tie of crossed quads - the thing that makes a
					// fast sword swipe look like a folded ribbon. Dropping the
					// point breaks the trail there, which is what it should look
					// like.
					if (havePrevious && along.Magnitude() > 1e-5f && previousAlong.Magnitude() > 1e-5f) {
						if (previousAlong.Unit().Dot(along.Unit()) < cosLimit) {
							// The visible gap in a fast swipe, and the only
							// evidence it was deliberate rather than a dropped
							// frame. Counted rather than logged per point: this
							// runs per recorded point per trail per frame.
							core::Metrics::Count("effects.trail.broken", 1.0);
							havePrevious = false;
							continue;
						}
					}
					previousAlong = along;
					havePrevious = true;
					travelled += along.Magnitude();
				}

				// **The width comes from the attachments and not from a
				// property**, which is Roblox's design and the right one: the
				// distance between the two attachments *is* how wide the trail is,
				// so widening it is moving them apart rather than editing a
				// number that then disagrees with where they are.
				const float age = trail.Lifetime > 0.0f ? trail.Age[at] / trail.Lifetime : 1.0f;
				const uint32_t colour =
					PackRgba(trail.Colour.Evaluate(age), 1.0f - trail.Transparency.Evaluate(age));

				const float repeat = trail.TextureLength > 0.0f ? travelled / trail.TextureLength : age;
				Emit(out, top, bottom, repeat, colour);
			}

			(void)eye;
		}

		struct FaceBasis {
			Vector3 Normal;
			Vector3 Right;
			Vector3 Up;
			float HalfWidth = 0.0f;
			float HalfHeight = 0.0f;
			float Depth = 0.0f;
		};

		FaceBasis BasisOf(scene::NormalId face, const scene::Bounds &bounds) {
			const Vector3 normal = scene::NormalOf(face);
			const Vector3 reference = std::abs(normal.Y) > 0.5f ? Vector3::ZAxis : Vector3::YAxis;
			const Vector3 right = reference.Cross(normal).Unit();
			const Vector3 up = normal.Cross(right).Unit();
			const auto reach = [&bounds](const Vector3 &axis) {
				return std::abs(axis.X) * bounds.HalfExtent.X + std::abs(axis.Y) * bounds.HalfExtent.Y +
					   std::abs(axis.Z) * bounds.HalfExtent.Z;
			};
			return FaceBasis{normal, right, up, reach(right), reach(up), reach(normal)};
		}

		template <class FaceImage>
		bool BuildFaceImage(
			const ecs::Store &store,
			ecs::Entity instance,
			const FaceImage &image,
			float u0,
			float v0,
			float u1,
			float v1,
			std::vector<RibbonVertex> &out
		) {
			const ecs::Entity parent = store.ParentOf(instance);
			const auto *transform = store.Get<scene::Transform>(parent);
			const auto *bounds = store.Get<scene::Bounds>(parent);
			if (transform == nullptr || bounds == nullptr || image.Transparency >= 1.0f) {
				return false;
			}

			const FaceBasis basis = BasisOf(image.Face, *bounds);
			const Vector3 centre = transform->Frame.PointToWorldSpace(basis.Normal * basis.Depth);
			const Vector3 right = transform->Frame.VectorToWorldSpace(basis.Right) * basis.HalfWidth;
			const Vector3 up = transform->Frame.VectorToWorldSpace(basis.Up) * basis.HalfHeight;
			const uint32_t colour = PackRgba(image.Colour, 1.0f - image.Transparency);

			out.push_back(RibbonVertex{centre - right + up, Vector2{u0, v0}, colour});
			out.push_back(RibbonVertex{centre - right - up, Vector2{u0, v1}, colour});
			out.push_back(RibbonVertex{centre + right + up, Vector2{u1, v0}, colour});
			out.push_back(RibbonVertex{centre + right - up, Vector2{u1, v1}, colour});
			return true;
		}
	}

	size_t BuildRibbons(ecs::Store &store, const Vector3 &eye, float elapsed) {
		ENGINE_PROFILE_CAT("build ribbons", core::ProfileCategory::Simulation);

		auto *buffer = store.ResourceMutable<RibbonBuffer>();
		if (buffer == nullptr) {
			return 0;
		}

		// Cleared rather than resized, because the vertex count is a function of
		// how many ribbons doubled back this frame and no count is known ahead of
		// the walk. The capacity stays, so a steady scene stops allocating after
		// its first frame - the same argument `engine::render::CollectInstances` makes for
		// its own buffer.
		buffer->Vertices.clear();
		buffer->Runs.clear();

		store.Each<const Beam>([&](ecs::Entity, const Beam &beam) {
			if (!beam.Enabled) {
				return;
			}

			const auto first = static_cast<uint32_t>(buffer->Vertices.size());
			BuildBeam(store, beam, eye, elapsed, buffer->Vertices);

			RibbonRun run;
			run.First = first;
			run.Count = static_cast<uint32_t>(buffer->Vertices.size()) - first;
			run.Texture = beam.Texture;
			run.ZOffset = beam.ZOffset;
			run.Additive = beam.Additive;
			if (run.Count >= 4) {
				buffer->Runs.push_back(run);
			} else {
				// An enabled beam that produced less than one quad draws
				// nothing, and the entity is simply absent from the frame.
				ENGINE_DEBUG_EVERY(5.0, "a beam produced {} vertices and was dropped", run.Count);
				buffer->Vertices.resize(first);
			}
		});

		store.Each<const Trail>([&](ecs::Entity, const Trail &trail) {
			const auto first = static_cast<uint32_t>(buffer->Vertices.size());
			BuildTrail(trail, eye, buffer->Vertices);

			RibbonRun run;
			run.First = first;
			run.Count = static_cast<uint32_t>(buffer->Vertices.size()) - first;
			run.Texture = trail.Texture;
			run.Additive = trail.Additive;
			if (run.Count >= 4) {
				buffer->Runs.push_back(run);
			} else {
				// **Rolled back rather than left as a one-pair run.** A run of two
				// vertices is half a quad, and a strip pass given one would read
				// past its own run to find the third. Cheaper to refuse it here
				// than to make every consumer check.
				//
				// A trail with fewer than two recorded points is the ordinary
				// case for a trail that has not moved yet, so this is `trace`
				// rather than a warning.
				ENGINE_TRACE("a trail produced {} vertices and was dropped", run.Count);
				buffer->Vertices.resize(first);
			}
		});

		store.Each<const Decal>([&](ecs::Entity entity, const Decal &decal) {
			const auto first = static_cast<uint32_t>(buffer->Vertices.size());
			if (!BuildFaceImage(store, entity, decal, 0.0f, 0.0f, 1.0f, 1.0f, buffer->Vertices)) {
				return;
			}

			RibbonRun run;
			run.First = first;
			run.Count = 4;
			run.Texture = decal.Image;
			run.ZOffset = static_cast<float>(std::max(decal.ZIndex, 0) + 1) * 0.0005f;
			buffer->Runs.push_back(run);
		});

		store.Each<const Texture>([&](ecs::Entity entity, const Texture &texture) {
			const ecs::Entity parent = store.ParentOf(entity);
			const auto *bounds = store.Get<scene::Bounds>(parent);
			if (bounds == nullptr) {
				return;
			}

			const FaceBasis basis = BasisOf(texture.Face, *bounds);
			const float tileU = std::max(texture.StudsPerTileU, 0.001f);
			const float tileV = std::max(texture.StudsPerTileV, 0.001f);
			const float u0 = texture.OffsetStudsU / tileU;
			const float v0 = texture.OffsetStudsV / tileV;
			const float u1 = u0 + basis.HalfWidth * 2.0f / tileU;
			const float v1 = v0 + basis.HalfHeight * 2.0f / tileV;

			const auto first = static_cast<uint32_t>(buffer->Vertices.size());
			if (!BuildFaceImage(store, entity, texture, u0, v0, u1, v1, buffer->Vertices)) {
				return;
			}

			RibbonRun run;
			run.First = first;
			run.Count = 4;
			run.Texture = texture.Image;
			run.ZOffset = static_cast<float>(std::max(texture.ZIndex, 0) + 1) * 0.0005f;
			run.RepeatV = true;
			buffer->Runs.push_back(run);
		});

		core::Metrics::SetGauge("effects.ribbon.runs", static_cast<double>(buffer->Runs.size()));
		core::Metrics::SetGauge("effects.ribbon.vertices", static_cast<double>(buffer->Vertices.size()));
		return buffer->Runs.size();
	}

	std::span<const RibbonVertex> RibbonStream(const ecs::Store &store) {
		const auto *buffer = store.Resource<RibbonBuffer>();
		return buffer == nullptr ? std::span<const RibbonVertex>{} : std::span(buffer->Vertices);
	}

	std::span<const RibbonRun> RibbonRuns(const ecs::Store &store) {
		const auto *buffer = store.Resource<RibbonBuffer>();
		return buffer == nullptr ? std::span<const RibbonRun>{} : std::span(buffer->Runs);
	}

	// --- serialisation --------------------------------------------------------
	//
	// Both hold a `core::Name` and both hold sequences whose unused tail is most
	// of their bytes, so both are written field by field - `Registration.cpp`
	// carries the whole argument for the emitter and it is the same one here.
	//
	// **A trail's recorded history is not written.** It is where something has
	// been over the last second, and a world restored from a file has not been
	// anywhere. Writing it would put a metre of sword swipe in the save file and
	// then draw it, motionless, at the position the world was saved from.

	namespace {
		void WriteRibbonGradient(core::ByteWriter &writer, const core::ColorSequence &sequence) {
			writer.WriteUInt32(sequence.Count);
			for (uint32_t index = 0; index < sequence.Count; index++) {
				writer.WriteFloat(sequence.Keypoints[index].Time);
				writer.WriteFloat(sequence.Keypoints[index].Value.R);
				writer.WriteFloat(sequence.Keypoints[index].Value.G);
				writer.WriteFloat(sequence.Keypoints[index].Value.B);
			}
		}

		void ReadRibbonGradient(core::ByteReader &reader, core::ColorSequence &sequence) {
			sequence = core::ColorSequence{};
			const uint32_t count = reader.ReadUInt32();
			for (uint32_t index = 0; index < count; index++) {
				const float time = reader.ReadFloat();
				const float red = reader.ReadFloat();
				const float green = reader.ReadFloat();
				const float blue = reader.ReadFloat();
				(void)sequence.Add(core::ColorKeypoint{time, core::Color3{red, green, blue}});
			}
		}

		void WriteCurve(core::ByteWriter &writer, const core::NumberSequence &sequence) {
			writer.WriteUInt32(sequence.Count);
			for (uint32_t index = 0; index < sequence.Count; index++) {
				writer.WriteFloat(sequence.Keypoints[index].Time);
				writer.WriteFloat(sequence.Keypoints[index].Value);
				writer.WriteFloat(sequence.Keypoints[index].Envelope);
			}
		}

		void ReadCurve(core::ByteReader &reader, core::NumberSequence &sequence) {
			sequence = core::NumberSequence{};
			const uint32_t count = reader.ReadUInt32();
			for (uint32_t index = 0; index < count; index++) {
				const float time = reader.ReadFloat();
				const float value = reader.ReadFloat();
				const float envelope = reader.ReadFloat();
				(void)sequence.Add(core::NumberKeypoint{time, value, envelope});
			}
		}
	}

	void WriteBeams(core::ByteWriter &writer, const void *source, size_t count) {
		const auto *beams = static_cast<const Beam *>(source);
		for (size_t index = 0; index < count; index++) {
			const Beam &beam = beams[index];
			WriteRibbonGradient(writer, beam.Colour);
			WriteCurve(writer, beam.Transparency);
			writer.WriteName(beam.Texture);

			// **The attachments do not cross and are cleared on the way in.** An
			// `ecs::Entity` is a handle within one world - rule 4 again - and a
			// document resolves references through its own local ids. A beam that
			// carried a raw handle across would point at whichever row took that
			// number.
			writer.WriteFloat(beam.CurveSize0);
			writer.WriteFloat(beam.CurveSize1);
			writer.WriteFloat(beam.Width0);
			writer.WriteFloat(beam.Width1);
			writer.WriteFloat(beam.TextureSpeed);
			writer.WriteFloat(beam.TextureLength);
			writer.WriteFloat(beam.ZOffset);
			writer.WriteBool(beam.FaceCamera);
			writer.WriteBool(beam.Additive);
			writer.WriteBool(beam.Enabled);
		}
	}

	void ReadBeams(core::ByteReader &reader, void *destination, size_t count) {
		auto *beams = static_cast<Beam *>(destination);
		for (size_t index = 0; index < count; index++) {
			Beam &beam = beams[index];
			ReadRibbonGradient(reader, beam.Colour);
			ReadCurve(reader, beam.Transparency);
			beam.Texture = reader.ReadName();

			beam.Attachment0 = ecs::NULL_ENTITY;
			beam.Attachment1 = ecs::NULL_ENTITY;

			beam.CurveSize0 = reader.ReadFloat();
			beam.CurveSize1 = reader.ReadFloat();
			beam.Width0 = reader.ReadFloat();
			beam.Width1 = reader.ReadFloat();
			beam.TextureSpeed = reader.ReadFloat();
			beam.TextureLength = reader.ReadFloat();
			beam.ZOffset = reader.ReadFloat();
			beam.FaceCamera = reader.ReadBool();
			beam.Additive = reader.ReadBool();
			beam.Enabled = reader.ReadBool();
		}
	}

	void WriteTrails(core::ByteWriter &writer, const void *source, size_t count) {
		const auto *trails = static_cast<const Trail *>(source);
		for (size_t index = 0; index < count; index++) {
			const Trail &trail = trails[index];
			WriteRibbonGradient(writer, trail.Colour);
			WriteCurve(writer, trail.Transparency);
			writer.WriteName(trail.Texture);
			writer.WriteFloat(trail.Lifetime);
			writer.WriteFloat(trail.MinimumAngle);
			writer.WriteFloat(trail.TextureLength);
			writer.WriteBool(trail.Enabled);
			writer.WriteBool(trail.Additive);
		}
	}

	void ReadTrails(core::ByteReader &reader, void *destination, size_t count) {
		auto *trails = static_cast<Trail *>(destination);
		for (size_t index = 0; index < count; index++) {
			Trail &trail = trails[index];
			ReadRibbonGradient(reader, trail.Colour);
			ReadCurve(reader, trail.Transparency);
			trail.Texture = reader.ReadName();
			trail.Lifetime = reader.ReadFloat();
			trail.MinimumAngle = reader.ReadFloat();
			trail.TextureLength = reader.ReadFloat();
			trail.Enabled = reader.ReadBool();
			trail.Additive = reader.ReadBool();

			trail.Attachment0 = ecs::NULL_ENTITY;
			trail.Attachment1 = ecs::NULL_ENTITY;
			trail.Head = 0;
			trail.Recorded = 0;
		}
	}

	void WriteDecals(core::ByteWriter &writer, const void *source, size_t count) {
		const auto *decals = static_cast<const Decal *>(source);
		for (size_t index = 0; index < count; index++) {
			const Decal &decal = decals[index];
			writer.WriteFloat(decal.Colour.R);
			writer.WriteFloat(decal.Colour.G);
			writer.WriteFloat(decal.Colour.B);
			writer.WriteName(decal.Image);
			writer.WriteFloat(decal.Transparency);
			writer.WriteInt32(decal.ZIndex);
			writer.WriteUInt8(static_cast<uint8_t>(decal.Face));
		}
	}

	void ReadDecals(core::ByteReader &reader, void *destination, size_t count) {
		auto *decals = static_cast<Decal *>(destination);
		for (size_t index = 0; index < count; index++) {
			Decal &decal = decals[index];
			decal.Colour = core::Color3{reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
			decal.Image = reader.ReadName();
			decal.Transparency = reader.ReadFloat();
			decal.ZIndex = reader.ReadInt32();
			decal.Face = static_cast<scene::NormalId>(reader.ReadUInt8());
		}
	}

	void WriteTextures(core::ByteWriter &writer, const void *source, size_t count) {
		const auto *textures = static_cast<const Texture *>(source);
		for (size_t index = 0; index < count; index++) {
			const Texture &texture = textures[index];
			writer.WriteFloat(texture.Colour.R);
			writer.WriteFloat(texture.Colour.G);
			writer.WriteFloat(texture.Colour.B);
			writer.WriteName(texture.Image);
			writer.WriteFloat(texture.Transparency);
			writer.WriteFloat(texture.StudsPerTileU);
			writer.WriteFloat(texture.StudsPerTileV);
			writer.WriteFloat(texture.OffsetStudsU);
			writer.WriteFloat(texture.OffsetStudsV);
			writer.WriteInt32(texture.ZIndex);
			writer.WriteUInt8(static_cast<uint8_t>(texture.Face));
		}
	}

	void ReadTextures(core::ByteReader &reader, void *destination, size_t count) {
		auto *textures = static_cast<Texture *>(destination);
		for (size_t index = 0; index < count; index++) {
			Texture &texture = textures[index];
			texture.Colour = core::Color3{reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
			texture.Image = reader.ReadName();
			texture.Transparency = reader.ReadFloat();
			texture.StudsPerTileU = reader.ReadFloat();
			texture.StudsPerTileV = reader.ReadFloat();
			texture.OffsetStudsU = reader.ReadFloat();
			texture.OffsetStudsV = reader.ReadFloat();
			texture.ZIndex = reader.ReadInt32();
			texture.Face = static_cast<scene::NormalId>(reader.ReadUInt8());
		}
	}
}
