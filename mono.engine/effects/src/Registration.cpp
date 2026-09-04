#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <array>
#include <string_view>

namespace engine::effects {

	namespace {
		using ecs::PropertyDescriptor;
		using ecs::PropertyKind;
		using ecs::PropertyType;

		// `ParticleEmitter` holds a `core::Name` - its texture - so it is written
		// with an explicit pair that writes the name as text. The raw object
		// representation would write the name's process-local id, which restores
		// in another process as whatever string happened to take that number.
		//
		// **Everything else is written field by field rather than as bytes**, and
		// the reason is not the name: it is that the type is fifteen hundred bytes
		// of which about a fifth is the unused tail of four fixed-capacity
		// sequences. Writing `Count` stops instead of twenty is most of a
		// kilobyte an emitter, on a component a hundred thousand rows of a world
		// may carry.
		void WriteSequence(core::ByteWriter &writer, const core::NumberSequence &sequence) {
			writer.WriteUInt32(sequence.Count);
			for (uint32_t index = 0; index < sequence.Count; index++) {
				writer.WriteFloat(sequence.Keypoints[index].Time);
				writer.WriteFloat(sequence.Keypoints[index].Value);
				writer.WriteFloat(sequence.Keypoints[index].Envelope);
			}
		}

		void ReadSequence(core::ByteReader &reader, core::NumberSequence &sequence) {
			sequence = core::NumberSequence{};
			const uint32_t count = reader.ReadUInt32();
			for (uint32_t index = 0; index < count; index++) {
				const float time = reader.ReadFloat();
				const float value = reader.ReadFloat();
				const float envelope = reader.ReadFloat();

				// **Dropped past capacity rather than refused**, which is the
				// opposite of what the script constructor does and is right for
				// the opposite reason: a script writing twenty-one keypoints has
				// made a mistake worth reporting, and a file holding twenty-one
				// was written by a build whose cap was higher. Refusing would make
				// the whole world unloadable over one gradient.
				if (!sequence.Add(core::NumberKeypoint{time, value, envelope})) {
					// The curve loads shorter than it was saved and the effect
					// looks subtly wrong, with nothing to tie it to the load.
					ENGINE_WARN(
						"a saved curve of {} keypoints is past this build's capacity; {} were dropped",
						count,
						count - sequence.Count
					);
					break;
				}
			}
		}

		void WriteGradient(core::ByteWriter &writer, const core::ColorSequence &sequence) {
			writer.WriteUInt32(sequence.Count);
			for (uint32_t index = 0; index < sequence.Count; index++) {
				writer.WriteFloat(sequence.Keypoints[index].Time);
				writer.WriteFloat(sequence.Keypoints[index].Value.R);
				writer.WriteFloat(sequence.Keypoints[index].Value.G);
				writer.WriteFloat(sequence.Keypoints[index].Value.B);
			}
		}

		void ReadGradient(core::ByteReader &reader, core::ColorSequence &sequence) {
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

		void WriteRange(core::ByteWriter &writer, const core::NumberRange &range) {
			writer.WriteFloat(range.Minimum);
			writer.WriteFloat(range.Maximum);
		}

		core::NumberRange ReadRange(core::ByteReader &reader) {
			const float minimum = reader.ReadFloat();
			return core::NumberRange{minimum, reader.ReadFloat()};
		}

		void WriteEmitters(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *emitters = static_cast<const ParticleEmitter *>(source);
			for (size_t index = 0; index < count; index++) {
				const ParticleEmitter &emitter = emitters[index];

				WriteSequence(writer, emitter.Size);
				WriteSequence(writer, emitter.Transparency);
				WriteSequence(writer, emitter.Squash);
				WriteGradient(writer, emitter.Colour);

				WriteRange(writer, emitter.Lifetime);
				WriteRange(writer, emitter.Speed);
				WriteRange(writer, emitter.Rotation);
				WriteRange(writer, emitter.RotationSpeed);
				WriteRange(writer, emitter.FlipbookFramerate);

				writer.WriteFloat(emitter.Acceleration.X);
				writer.WriteFloat(emitter.Acceleration.Y);
				writer.WriteFloat(emitter.Acceleration.Z);
				writer.WriteFloat(emitter.SpreadAngle.X);
				writer.WriteFloat(emitter.SpreadAngle.Y);

				writer.WriteName(emitter.Texture);
				writer.WriteInt32(emitter.MaxParticles);

				writer.WriteFloat(emitter.Rate);
				writer.WriteFloat(emitter.RateOverDistance);
				writer.WriteFloat(emitter.Drag);
				writer.WriteFloat(emitter.MaxSpeed);
				writer.WriteFloat(emitter.NoiseStrength);
				writer.WriteFloat(emitter.NoiseFrequency);
				writer.WriteFloat(emitter.NoiseScrollSpeed);
				writer.WriteFloat(emitter.RadialAcceleration);
				writer.WriteFloat(emitter.TangentialAcceleration);
				writer.WriteFloat(emitter.VelocityInheritance);
				writer.WriteFloat(emitter.LightEmission);
				writer.WriteFloat(emitter.LightInfluence);
				writer.WriteFloat(emitter.Brightness);
				writer.WriteFloat(emitter.ShapePartial);
				writer.WriteFloat(emitter.ZOffset);
				writer.WriteFloat(emitter.TimeScale);

				writer.WriteUInt8(static_cast<uint8_t>(emitter.EmissionDirection));
				writer.WriteUInt8(static_cast<uint8_t>(emitter.Orientation));
				writer.WriteUInt8(static_cast<uint8_t>(emitter.Shape));
				writer.WriteUInt8(static_cast<uint8_t>(emitter.ShapeStyle));
				writer.WriteUInt8(static_cast<uint8_t>(emitter.ShapeDirection));
				writer.WriteUInt8(static_cast<uint8_t>(emitter.Flipbook));
				writer.WriteUInt8(static_cast<uint8_t>(emitter.FlipbookPlayback));
				writer.WriteUInt8(emitter.FlipbookFrames);
				writer.WriteBool(emitter.FlipbookStartRandom);
				writer.WriteBool(emitter.LockedToPart);
				writer.WriteBool(emitter.Enabled);
				writer.WriteBool(emitter.Additive);
				writer.WriteBool(emitter.SoftParticles);
			}
		}

		void ReadEmitters(core::ByteReader &reader, void *destination, size_t count) {
			auto *emitters = static_cast<ParticleEmitter *>(destination);
			for (size_t index = 0; index < count; index++) {
				ParticleEmitter &emitter = emitters[index];

				ReadSequence(reader, emitter.Size);
				ReadSequence(reader, emitter.Transparency);
				ReadSequence(reader, emitter.Squash);
				ReadGradient(reader, emitter.Colour);

				emitter.Lifetime = ReadRange(reader);
				emitter.Speed = ReadRange(reader);
				emitter.Rotation = ReadRange(reader);
				emitter.RotationSpeed = ReadRange(reader);
				emitter.FlipbookFramerate = ReadRange(reader);

				emitter.Acceleration.X = reader.ReadFloat();
				emitter.Acceleration.Y = reader.ReadFloat();
				emitter.Acceleration.Z = reader.ReadFloat();
				emitter.SpreadAngle.X = reader.ReadFloat();
				emitter.SpreadAngle.Y = reader.ReadFloat();

				emitter.Texture = reader.ReadName();
				emitter.MaxParticles = reader.ReadInt32();

				emitter.Rate = reader.ReadFloat();
				emitter.RateOverDistance = reader.ReadFloat();
				emitter.Drag = reader.ReadFloat();
				emitter.MaxSpeed = reader.ReadFloat();
				emitter.NoiseStrength = reader.ReadFloat();
				emitter.NoiseFrequency = reader.ReadFloat();
				emitter.NoiseScrollSpeed = reader.ReadFloat();
				emitter.RadialAcceleration = reader.ReadFloat();
				emitter.TangentialAcceleration = reader.ReadFloat();
				emitter.VelocityInheritance = reader.ReadFloat();
				emitter.LightEmission = reader.ReadFloat();
				emitter.LightInfluence = reader.ReadFloat();
				emitter.Brightness = reader.ReadFloat();
				emitter.ShapePartial = reader.ReadFloat();
				emitter.ZOffset = reader.ReadFloat();
				emitter.TimeScale = reader.ReadFloat();

				emitter.EmissionDirection = static_cast<scene::NormalId>(reader.ReadUInt8());
				emitter.Orientation = static_cast<ParticleOrientation>(reader.ReadUInt8());
				emitter.Shape = static_cast<ParticleShape>(reader.ReadUInt8());
				emitter.ShapeStyle = static_cast<ParticleShapeStyle>(reader.ReadUInt8());
				emitter.ShapeDirection = static_cast<ParticleShapeDirection>(reader.ReadUInt8());
				emitter.Flipbook = static_cast<FlipbookLayout>(reader.ReadUInt8());
				emitter.FlipbookPlayback = static_cast<FlipbookMode>(reader.ReadUInt8());
				emitter.FlipbookFrames = reader.ReadUInt8();
				emitter.FlipbookStartRandom = reader.ReadBool();
				emitter.LockedToPart = reader.ReadBool();
				emitter.Enabled = reader.ReadBool();
				emitter.Additive = reader.ReadBool();
				emitter.SoftParticles = reader.ReadBool();
			}
		}

		// The pool is derived state and its serialisation says so by writing
		// nothing - `engine::render::DrawList`'s argument, applied to something far bigger.
		//
		// A world's particles are its emitters plus one frame of simulation, and
		// writing half a million of them into every save file would be storing an
		// answer that is recomputed before it is ever read. What is *not* recovered
		// is the particles that were in the air when the world was saved, and that
		// is the honest cost: reloading a scene restarts its effects. Roblox does
		// the same.
		void WriteSystems(core::ByteWriter &, const void *, size_t) {}

		void ReadSystems(core::ByteReader &, void *destination, size_t count) {
			auto *systems = static_cast<ParticleSystem *>(destination);
			for (size_t index = 0; index < count; index++) {
				systems[index].Blocks.clear();
				systems[index].FrameParents.clear();
				systems[index].TextureRevision = 0;
				systems[index].Free.clear();
				systems[index].Used = 0;
				systems[index].RetryRefused = false;
				systems[index].Statistics = {};
			}
		}

		// A slot is a position in one process's pool. Restoring it would point an
		// emitter at whatever block took that number - rule 4's hazard, so nothing
		// crosses and the reader clears.
		void WriteSlots(core::ByteWriter &, const void *, size_t) {}

		void ReadSlots(core::ByteReader &, void *destination, size_t count) {
			auto *slots = static_cast<EmitterSlot *>(destination);
			for (size_t index = 0; index < count; index++) {
				slots[index].Requested = 0;
				slots[index].Index = NO_SLOT;
				slots[index].Enabled = true;
				slots[index].Configured = false;
				slots[index].ClearRequested = false;
				slots[index].Refused = false;
			}
		}
	}

	namespace {
		void RegisterEffectComponentsOnce() {
			scene::RegisterSceneComponents();

			ecs::Components::Register<ParticleEmitter>(
				"effects.ParticleEmitter", WriteEmitters, ReadEmitters
			);
			ecs::Components::Register<EmitterSlot>("effects.EmitterSlot", WriteSlots, ReadSlots);
			ecs::Components::Register<Beam>("effects.Beam", WriteBeams, ReadBeams);
			ecs::Components::Register<Trail>("effects.Trail", WriteTrails, ReadTrails);
			ecs::Components::Register<Decal>("effects.Decal", WriteDecals, ReadDecals);
			ecs::Components::Register<Texture>("effects.Texture", WriteTextures, ReadTextures);
			ecs::Components::Register<ParticleSystem>("effects.ParticleSystem", WriteSystems, ReadSystems);

			// **The ribbon buffer, and forgetting it was caught by a snapshot test
			// rather than by a compiler.** `Store::Save` refuses a resource with no
			// serialisation instead of writing bytes it cannot read back, so a world
			// with a `RibbonBuffer` in it simply would not save - which is what
			// `client/tests/Presentation.cpp` reported, in exactly the words
			// `engine::render::DrawList`'s own comment predicts for this mistake.
			//
			// Nothing is written, for `DrawList`'s reason: the vertices are rebuilt by
			// `BuildRibbons` in `PreRender` every frame before anything looks at them,
			// so writing a frame's worth of them into every save file would be storing
			// an answer that is recomputed before it is used.
			ecs::Components::Register<RibbonBuffer>(
				"effects.RibbonBuffer",
				[](core::ByteWriter &, const void *, size_t) {},
				[](core::ByteReader &, void *destination, size_t count) {
					auto *buffers = static_cast<RibbonBuffer *>(destination);
					for (size_t index = 0; index < count; index++) {
						buffers[index].Vertices.clear();
						buffers[index].Runs.clear();
					}
				}
			);
		}
	}

	void RegisterEffectComponents() {
		// Script runtimes need neutral particle methods before they construct an
		// effect class. Component registration therefore has more than one valid
		// entry path and must preserve the public idempotence contract.
		static const bool registered = [] {
			RegisterEffectComponentsOnce();
			return true;
		}();
		(void)registered;
	}
}
