#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>

namespace engine::ecs {

	bool AttributeTypeAllowed(PropertyType type) {
		// A closed refusal rather than a closed acceptance, so a type added to
		// `PropertyType` is storable by default. That is the right direction:
		// the list exists to describe what userland can hold, and an attribute is
		// the least restricted place userland holds anything.
		return type != PropertyType::Reference && type != PropertyType::Opaque;
	}

	namespace {
		// The table, or null. Const path - never creates one.
		const AttributeTable *TableOf(const Store &store) {
			return store.Resource<AttributeTable>();
		}
	}

	bool GetAttribute(const Store &store, Entity instance, core::Name name, AttributeValue &out) {
		const AttributeTable *table = TableOf(store);
		if (table == nullptr || !name.IsValid()) {
			return false;
		}

		const auto entity = table->Entities.find(instance.Id);
		if (entity == table->Entities.end()) {
			return false;
		}

		const auto found = entity->second.find(name.Id());
		if (found == entity->second.end()) {
			return false;
		}

		out = found->second;
		return true;
	}

	bool SetAttribute(Store &store, Entity instance, core::Name name, const AttributeValue &value) {
		if (!name.IsValid() || !store.Alive(instance)) {
			return false;
		}

		// **Removal first, before the table is created.** Clearing an attribute on
		// a world that has never had one should not leave an empty table behind -
		// and more usefully, it means `SetAttribute(name, nil)` is safe on any
		// instance rather than only on ones that have been written to.
		if (value.Type == PropertyType::Opaque) {
			auto *table = store.ResourceMutable<AttributeTable>();
			if (table == nullptr) {
				return true;
			}

			const auto entity = table->Entities.find(instance.Id);
			if (entity == table->Entities.end()) {
				return true;
			}

			entity->second.erase(name.Id());

			// **The entity's map is dropped when it empties**, so a world that
			// sets and clears attributes in a loop does not accumulate one empty
			// map per instance it ever touched.
			if (entity->second.empty()) {
				table->Entities.erase(entity);
			}
			return true;
		}

		if (!AttributeTypeAllowed(value.Type)) {
			return false;
		}

		if (!store.HasResource<AttributeTable>()) {
			// Created on first write rather than by whatever furnishes a world,
			// so a world with no attributes carries no table - which is what makes
			// the "costs nothing at all" claim in the header true.
			store.SetResource(AttributeTable{});
		}

		auto *table = store.ResourceMutable<AttributeTable>();
		if (table == nullptr) {
			// An adopt-only replica refuses `SetResource`. Reported rather than
			// asserted: a script running on a replica writing an attribute is an
			// ordinary mistake and not a fault.
			return false;
		}

		table->Entities[instance.Id][name.Id()] = value;
		return true;
	}

	std::vector<core::Name> AttributeNames(const Store &store, Entity instance) {
		std::vector<core::Name> names;

		const AttributeTable *table = TableOf(store);
		if (table == nullptr) {
			return names;
		}

		const auto entity = table->Entities.find(instance.Id);
		if (entity == table->Entities.end()) {
			return names;
		}

		names.reserve(entity->second.size());
		for (const auto &[id, value] : entity->second) {
			(void)value;
			names.push_back(core::Name::FromId(id));
		}

		// By text, not by id: an id is a first-seen counter and its order is
		// whatever the process happened to intern in, which differs between two
		// runs of one scene.
		std::sort(names.begin(), names.end(), [](const core::Name &left, const core::Name &right) {
			return left.Text() < right.Text();
		});
		return names;
	}

	size_t ClearAttributes(Store &store, Entity instance) {
		auto *table = store.ResourceMutable<AttributeTable>();
		if (table == nullptr) {
			return 0;
		}

		const auto entity = table->Entities.find(instance.Id);
		if (entity == table->Entities.end()) {
			return 0;
		}

		const size_t dropped = entity->second.size();
		table->Entities.erase(entity);
		return dropped;
	}

	namespace {
		// The wire form.
		//
		// **Both keys cross carefully and for different reasons.** A `core::Name`
		// id is a first-seen counter, so the name is written as *text* - rule 4,
		// exactly as every other name in the engine crosses. An entity id is a
		// slot this world allocated, and it is written raw, which is legal here
		// and only here: a snapshot restores a *whole world* including its entity
		// ids, so the number means the same thing on the way back in. A `game`
		// document, which does not preserve ids, resolves references through its
		// own local table and never reaches this pair.
		//
		// **Field by field and never as an object representation**, which is the
		// rule `scene::WriteVisuals` follows for holding a name and which applies
		// here for a second reason too: a `NumberSequence` is 248 bytes of which
		// the unused tail is most of it, and writing `Count` stops is the
		// difference between a save file that carries a gradient and one that
		// carries twenty slots to say it.
		void WriteValue(core::ByteWriter &writer, const AttributeValue &value) {
			writer.WriteUInt8(static_cast<uint8_t>(value.Type));

			switch (value.Type) {
			case PropertyType::Bool:
				writer.WriteBool(value.Bool);
				break;
			case PropertyType::Int32:
				writer.WriteInt32(value.Int32);
				break;
			case PropertyType::Int64:
				writer.WriteInt64(value.Int64);
				break;
			case PropertyType::Float:
				writer.WriteFloat(value.Float);
				break;
			case PropertyType::Double:
				writer.WriteDouble(value.Double);
				break;
			case PropertyType::Name:
			case PropertyType::Enum:
				writer.WriteName(value.Name);
				break;
			case PropertyType::String:
				writer.WriteString(value.String);
				break;
			case PropertyType::Vector3:
				writer.WriteFloat(value.Vector3.X);
				writer.WriteFloat(value.Vector3.Y);
				writer.WriteFloat(value.Vector3.Z);
				break;
			case PropertyType::Color3:
				writer.WriteFloat(value.Color3.R);
				writer.WriteFloat(value.Color3.G);
				writer.WriteFloat(value.Color3.B);
				break;
			case PropertyType::CFrame:
				writer.WriteFloat(value.CFrame.Position.X);
				writer.WriteFloat(value.CFrame.Position.Y);
				writer.WriteFloat(value.CFrame.Position.Z);
				writer.WriteFloat(value.CFrame.QuaternionX);
				writer.WriteFloat(value.CFrame.QuaternionY);
				writer.WriteFloat(value.CFrame.QuaternionZ);
				writer.WriteFloat(value.CFrame.QuaternionW);
				break;
			case PropertyType::Vector2:
				writer.WriteFloat(value.Vector2.X);
				writer.WriteFloat(value.Vector2.Y);
				break;
			case PropertyType::UDim:
				writer.WriteFloat(value.UDim.Scale);
				writer.WriteFloat(value.UDim.Offset);
				break;
			case PropertyType::UDim2:
				writer.WriteFloat(value.UDim2.X.Scale);
				writer.WriteFloat(value.UDim2.X.Offset);
				writer.WriteFloat(value.UDim2.Y.Scale);
				writer.WriteFloat(value.UDim2.Y.Offset);
				break;
			case PropertyType::Rect:
				writer.WriteFloat(value.Rect.Min.X);
				writer.WriteFloat(value.Rect.Min.Y);
				writer.WriteFloat(value.Rect.Max.X);
				writer.WriteFloat(value.Rect.Max.Y);
				break;
			case PropertyType::NumberRange:
				writer.WriteFloat(value.NumberRange.Minimum);
				writer.WriteFloat(value.NumberRange.Maximum);
				break;
			case PropertyType::NumberSequence:
				writer.WriteUInt32(value.NumberSequence.Count);
				for (uint32_t index = 0; index < value.NumberSequence.Count; index++) {
					const core::NumberKeypoint &stop = value.NumberSequence.Keypoints[index];
					writer.WriteFloat(stop.Time);
					writer.WriteFloat(stop.Value);
					writer.WriteFloat(stop.Envelope);
				}
				break;
			case PropertyType::ColorSequence:
				writer.WriteUInt32(value.ColorSequence.Count);
				for (uint32_t index = 0; index < value.ColorSequence.Count; index++) {
					const core::ColorKeypoint &stop = value.ColorSequence.Keypoints[index];
					writer.WriteFloat(stop.Time);
					writer.WriteFloat(stop.Value.R);
					writer.WriteFloat(stop.Value.G);
					writer.WriteFloat(stop.Value.B);
				}
				break;
			case PropertyType::Reference:
			case PropertyType::Opaque:
				// Never stored - `AttributeTypeAllowed` refuses both - so nothing
				// is written and the reader writes nothing back.
				break;
			}
		}

		AttributeValue ReadValue(core::ByteReader &reader) {
			AttributeValue value;
			const auto type = static_cast<PropertyType>(reader.ReadUInt8());
			value.Type = type;

			switch (type) {
			case PropertyType::Bool:
				value.Bool = reader.ReadBool();
				break;
			case PropertyType::Int32:
				value.Int32 = reader.ReadInt32();
				break;
			case PropertyType::Int64:
				value.Int64 = reader.ReadInt64();
				break;
			case PropertyType::Float:
				value.Float = reader.ReadFloat();
				break;
			case PropertyType::Double:
				value.Double = reader.ReadDouble();
				break;
			case PropertyType::Name:
			case PropertyType::Enum:
				value.Name = reader.ReadName();
				break;
			case PropertyType::String:
				value.String = std::string(reader.ReadString());
				break;
			case PropertyType::Vector3:
				value.Vector3.X = reader.ReadFloat();
				value.Vector3.Y = reader.ReadFloat();
				value.Vector3.Z = reader.ReadFloat();
				break;
			case PropertyType::Color3:
				value.Color3.R = reader.ReadFloat();
				value.Color3.G = reader.ReadFloat();
				value.Color3.B = reader.ReadFloat();
				break;
			case PropertyType::CFrame:
				value.CFrame.Position.X = reader.ReadFloat();
				value.CFrame.Position.Y = reader.ReadFloat();
				value.CFrame.Position.Z = reader.ReadFloat();
				value.CFrame.QuaternionX = reader.ReadFloat();
				value.CFrame.QuaternionY = reader.ReadFloat();
				value.CFrame.QuaternionZ = reader.ReadFloat();
				value.CFrame.QuaternionW = reader.ReadFloat();
				break;
			case PropertyType::Vector2:
				value.Vector2.X = reader.ReadFloat();
				value.Vector2.Y = reader.ReadFloat();
				break;
			case PropertyType::UDim:
				value.UDim.Scale = reader.ReadFloat();
				value.UDim.Offset = reader.ReadFloat();
				break;
			case PropertyType::UDim2:
				value.UDim2.X.Scale = reader.ReadFloat();
				value.UDim2.X.Offset = reader.ReadFloat();
				value.UDim2.Y.Scale = reader.ReadFloat();
				value.UDim2.Y.Offset = reader.ReadFloat();
				break;
			case PropertyType::Rect:
				value.Rect.Min.X = reader.ReadFloat();
				value.Rect.Min.Y = reader.ReadFloat();
				value.Rect.Max.X = reader.ReadFloat();
				value.Rect.Max.Y = reader.ReadFloat();
				break;
			case PropertyType::NumberRange: {
				const float minimum = reader.ReadFloat();
				value.NumberRange = core::NumberRange{minimum, reader.ReadFloat()};
				break;
			}
			case PropertyType::NumberSequence: {
				const uint32_t count = reader.ReadUInt32();
				for (uint32_t index = 0; index < count; index++) {
					const float time = reader.ReadFloat();
					const float number = reader.ReadFloat();
					const float envelope = reader.ReadFloat();

					// Dropped past capacity rather than refused, for
					// `effects::ReadSequence`'s reason: a file holding twenty-one
					// keypoints was written by a build whose cap was higher, and
					// refusing would make the whole world unloadable over one
					// gradient.
					(void)value.NumberSequence.Add(core::NumberKeypoint{time, number, envelope});
				}
				break;
			}
			case PropertyType::ColorSequence: {
				const uint32_t count = reader.ReadUInt32();
				for (uint32_t index = 0; index < count; index++) {
					const float time = reader.ReadFloat();
					const float red = reader.ReadFloat();
					const float green = reader.ReadFloat();
					const float blue = reader.ReadFloat();
					(void)value.ColorSequence.Add(core::ColorKeypoint{time, core::Color3{red, green, blue}});
				}
				break;
			}
			case PropertyType::Reference:
			case PropertyType::Opaque:
				break;
			}

			return value;
		}

		void WriteTables(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *tables = static_cast<const AttributeTable *>(source);
			for (size_t index = 0; index < count; index++) {
				const AttributeTable &table = tables[index];

				writer.WriteUInt32(static_cast<uint32_t>(table.Entities.size()));
				for (const auto &[entity, attributes] : table.Entities) {
					writer.WriteUInt32(entity);
					writer.WriteUInt32(static_cast<uint32_t>(attributes.size()));

					for (const auto &[nameId, value] : attributes) {
						writer.WriteName(core::Name::FromId(nameId));
						WriteValue(writer, value);
					}
				}
			}
		}

		void ReadTables(core::ByteReader &reader, void *destination, size_t count) {
			auto *tables = static_cast<AttributeTable *>(destination);
			for (size_t index = 0; index < count; index++) {
				AttributeTable &table = tables[index];
				table.Entities.clear();

				const uint32_t entities = reader.ReadUInt32();
				for (uint32_t entity = 0; entity < entities; entity++) {
					const uint32_t id = reader.ReadUInt32();
					const uint32_t attributes = reader.ReadUInt32();

					for (uint32_t attribute = 0; attribute < attributes; attribute++) {
						const core::Name name = reader.ReadName();
						AttributeValue value = ReadValue(reader);

						// **Interned on the way in, which is what makes the id a
						// valid key.** The name crossed as text and
						// `ReadName` interned it in *this* process, so the id it
						// has now is this process's - which is the whole point of
						// writing the text.
						if (name.IsValid()) {
							table.Entities[id][name.Id()] = std::move(value);
						}
					}
				}
			}
		}
	}

	void RegisterAttributeComponents() {
		Components::Register<AttributeTable>("ecs.AttributeTable", WriteTables, ReadTables);
	}
}
