#include <engine/ecs/Components.hpp>
#include <engine/gui/VirtualCollection.hpp>

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_set>
#include <utf8proc.h>
#include <utility>

namespace engine::gui {
	namespace {
		template <class T> void WriteTransientVirtualState(core::ByteWriter &, const void *, size_t) {}

		template <class T>
		void ReadTransientVirtualState(core::ByteReader &, void *destination, size_t count) {
			auto *values = static_cast<T *>(destination);
			for (size_t index = 0; index < count; index++)
				values[index] = {};
		}

		bool ValidUtf8(std::string_view text) {
			for (size_t offset = 0; offset < text.size();) {
				utf8proc_int32_t codepoint = 0;
				const utf8proc_ssize_t consumed = utf8proc_iterate(
					reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset),
					static_cast<utf8proc_ssize_t>(text.size() - offset),
					&codepoint
				);
				if (consumed <= 0) {
					return false;
				}
				offset += static_cast<size_t>(consumed);
			}
			return true;
		}

		bool ValidText(std::string_view text) {
			return !text.empty() && text.size() <= VirtualCollection::MAXIMUM_TEXT_BYTES && ValidUtf8(text);
		}

		bool ValidExtent(float extent) {
			return std::isfinite(extent) && extent > 0.0f;
		}

		bool SerializableValueType(ecs::PropertyType type) {
			switch (type) {
			case ecs::PropertyType::Bool:
			case ecs::PropertyType::Int32:
			case ecs::PropertyType::Int64:
			case ecs::PropertyType::Float:
			case ecs::PropertyType::Double:
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
			case ecs::PropertyType::String:
			case ecs::PropertyType::Vector3:
			case ecs::PropertyType::CFrame:
			case ecs::PropertyType::Color3:
			case ecs::PropertyType::Vector2:
			case ecs::PropertyType::UDim:
			case ecs::PropertyType::UDim2:
			case ecs::PropertyType::Rect:
			case ecs::PropertyType::NumberRange:
			case ecs::PropertyType::NumberSequence:
			case ecs::PropertyType::ColorSequence:
				return true;
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				return false;
			}
			return false;
		}

		bool Finite(const core::Vector2 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y);
		}

		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		bool Finite(const core::Color3 &value) {
			return std::isfinite(value.R) && std::isfinite(value.G) && std::isfinite(value.B);
		}

		bool ValidValue(const ecs::AttributeValue &value) {
			if (!SerializableValueType(value.Type)) {
				return false;
			}
			switch (value.Type) {
			case ecs::PropertyType::Bool:
			case ecs::PropertyType::Int32:
			case ecs::PropertyType::Int64:
				return true;
			case ecs::PropertyType::Float:
				return std::isfinite(value.Float);
			case ecs::PropertyType::Double:
				return std::isfinite(value.Double);
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
				return value.Name.IsValid() && ValidText(value.Name.Text());
			case ecs::PropertyType::String:
				return value.String.size() <= VirtualCollection::MAXIMUM_TEXT_BYTES &&
					   ValidUtf8(value.String);
			case ecs::PropertyType::Vector3:
				return Finite(value.Vector3);
			case ecs::PropertyType::CFrame:
				return Finite(value.CFrame.Position) && std::isfinite(value.CFrame.QuaternionX) &&
					   std::isfinite(value.CFrame.QuaternionY) && std::isfinite(value.CFrame.QuaternionZ) &&
					   std::isfinite(value.CFrame.QuaternionW);
			case ecs::PropertyType::Color3:
				return Finite(value.Color3);
			case ecs::PropertyType::Vector2:
				return Finite(value.Vector2);
			case ecs::PropertyType::UDim:
				return std::isfinite(value.UDim.Scale) && std::isfinite(value.UDim.Offset);
			case ecs::PropertyType::UDim2:
				return std::isfinite(value.UDim2.X.Scale) && std::isfinite(value.UDim2.X.Offset) &&
					   std::isfinite(value.UDim2.Y.Scale) && std::isfinite(value.UDim2.Y.Offset);
			case ecs::PropertyType::Rect:
				return Finite(value.Rect.Min) && Finite(value.Rect.Max);
			case ecs::PropertyType::NumberRange:
				return std::isfinite(value.NumberRange.Minimum) && std::isfinite(value.NumberRange.Maximum);
			case ecs::PropertyType::NumberSequence:
				if (value.NumberSequence.Count > core::SEQUENCE_CAPACITY) {
					return false;
				}
				for (uint32_t index = 0; index < value.NumberSequence.Count; index++) {
					const core::NumberKeypoint &keypoint = value.NumberSequence.Keypoints[index];
					if (!std::isfinite(keypoint.Time) || !std::isfinite(keypoint.Value) ||
						!std::isfinite(keypoint.Envelope)) {
						return false;
					}
				}
				return true;
			case ecs::PropertyType::ColorSequence:
				if (value.ColorSequence.Count > core::SEQUENCE_CAPACITY) {
					return false;
				}
				for (uint32_t index = 0; index < value.ColorSequence.Count; index++) {
					const core::ColorKeypoint &keypoint = value.ColorSequence.Keypoints[index];
					if (!std::isfinite(keypoint.Time) || !Finite(keypoint.Value)) {
						return false;
					}
				}
				return true;
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				return false;
			}
			return false;
		}

		bool AddBytes(size_t &total, size_t bytes) {
			if (bytes > VirtualCollection::MAXIMUM_PAGE_BYTES - total) {
				return false;
			}
			total += bytes;
			return true;
		}

		bool MeasureValue(const ecs::AttributeValue &value, size_t &total) {
			if (!AddBytes(total, 1)) {
				return false;
			}
			switch (value.Type) {
			case ecs::PropertyType::Bool:
				return AddBytes(total, 1);
			case ecs::PropertyType::Int32:
			case ecs::PropertyType::Float:
				return AddBytes(total, 4);
			case ecs::PropertyType::Int64:
			case ecs::PropertyType::Double:
				return AddBytes(total, 8);
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
				return AddBytes(total, 4 + value.Name.Text().size());
			case ecs::PropertyType::String:
				return AddBytes(total, 4 + value.String.size());
			case ecs::PropertyType::Vector3:
			case ecs::PropertyType::Color3:
				return AddBytes(total, 12);
			case ecs::PropertyType::CFrame:
				return AddBytes(total, 28);
			case ecs::PropertyType::Vector2:
			case ecs::PropertyType::UDim:
			case ecs::PropertyType::NumberRange:
				return AddBytes(total, 8);
			case ecs::PropertyType::UDim2:
			case ecs::PropertyType::Rect:
				return AddBytes(total, 16);
			case ecs::PropertyType::NumberSequence:
				return AddBytes(total, 4 + 12 * value.NumberSequence.Count);
			case ecs::PropertyType::ColorSequence:
				return AddBytes(total, 4 + 16 * value.ColorSequence.Count);
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				return false;
			}
			return false;
		}

		bool WithinBudget(const core::ByteReader &reader, size_t start) {
			return reader.Position() - start <= VirtualCollection::MAXIMUM_PAGE_BYTES;
		}

		bool MeasureCollection(const VirtualCollection &collection) {
			// The collection prelude ends with the record count. Keeping this
			// arithmetic beside WriteVirtualCollection makes the declared page
			// ceiling a bound on emitted bytes rather than an estimate.
			size_t total = 1 + 4 + 1 + 4 + 4 + 4 + 4 + 8 + 4 + 4 + 4 + 4;
			for (const VirtualRecord &record : collection.Page.Records) {
				if (!AddBytes(total, 4 + record.Key.size() + 4 + 4)) {
					return false;
				}
				for (const VirtualField &field : record.Fields) {
					if (!AddBytes(total, 4 + field.Name.size()) || !MeasureValue(field.Value, total)) {
						return false;
					}
				}
			}
			return true;
		}

		void WriteValue(core::ByteWriter &writer, const ecs::AttributeValue &value) {
			writer.WriteUInt8(static_cast<uint8_t>(value.Type));
			switch (value.Type) {
			case ecs::PropertyType::Bool:
				writer.WriteBool(value.Bool);
				break;
			case ecs::PropertyType::Int32:
				writer.WriteInt32(value.Int32);
				break;
			case ecs::PropertyType::Int64:
				writer.WriteInt64(value.Int64);
				break;
			case ecs::PropertyType::Float:
				writer.WriteFloat(value.Float);
				break;
			case ecs::PropertyType::Double:
				writer.WriteDouble(value.Double);
				break;
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
				writer.WriteName(value.Name);
				break;
			case ecs::PropertyType::String:
				writer.WriteString(value.String);
				break;
			case ecs::PropertyType::Vector3:
				writer.WriteFloat(value.Vector3.X);
				writer.WriteFloat(value.Vector3.Y);
				writer.WriteFloat(value.Vector3.Z);
				break;
			case ecs::PropertyType::CFrame:
				writer.WriteFloat(value.CFrame.Position.X);
				writer.WriteFloat(value.CFrame.Position.Y);
				writer.WriteFloat(value.CFrame.Position.Z);
				writer.WriteFloat(value.CFrame.QuaternionX);
				writer.WriteFloat(value.CFrame.QuaternionY);
				writer.WriteFloat(value.CFrame.QuaternionZ);
				writer.WriteFloat(value.CFrame.QuaternionW);
				break;
			case ecs::PropertyType::Color3:
				writer.WriteFloat(value.Color3.R);
				writer.WriteFloat(value.Color3.G);
				writer.WriteFloat(value.Color3.B);
				break;
			case ecs::PropertyType::Vector2:
				writer.WriteFloat(value.Vector2.X);
				writer.WriteFloat(value.Vector2.Y);
				break;
			case ecs::PropertyType::UDim:
				writer.WriteFloat(value.UDim.Scale);
				writer.WriteFloat(value.UDim.Offset);
				break;
			case ecs::PropertyType::UDim2:
				writer.WriteFloat(value.UDim2.X.Scale);
				writer.WriteFloat(value.UDim2.X.Offset);
				writer.WriteFloat(value.UDim2.Y.Scale);
				writer.WriteFloat(value.UDim2.Y.Offset);
				break;
			case ecs::PropertyType::Rect:
				writer.WriteFloat(value.Rect.Min.X);
				writer.WriteFloat(value.Rect.Min.Y);
				writer.WriteFloat(value.Rect.Max.X);
				writer.WriteFloat(value.Rect.Max.Y);
				break;
			case ecs::PropertyType::NumberRange:
				writer.WriteFloat(value.NumberRange.Minimum);
				writer.WriteFloat(value.NumberRange.Maximum);
				break;
			case ecs::PropertyType::NumberSequence:
				writer.WriteUInt32(value.NumberSequence.Count);
				for (uint32_t index = 0; index < value.NumberSequence.Count; index++) {
					const core::NumberKeypoint &keypoint = value.NumberSequence.Keypoints[index];
					writer.WriteFloat(keypoint.Time);
					writer.WriteFloat(keypoint.Value);
					writer.WriteFloat(keypoint.Envelope);
				}
				break;
			case ecs::PropertyType::ColorSequence:
				writer.WriteUInt32(value.ColorSequence.Count);
				for (uint32_t index = 0; index < value.ColorSequence.Count; index++) {
					const core::ColorKeypoint &keypoint = value.ColorSequence.Keypoints[index];
					writer.WriteFloat(keypoint.Time);
					writer.WriteFloat(keypoint.Value.R);
					writer.WriteFloat(keypoint.Value.G);
					writer.WriteFloat(keypoint.Value.B);
				}
				break;
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				break;
			}
		}

		ecs::AttributeValue ReadValue(core::ByteReader &reader, size_t start) {
			ecs::AttributeValue value;
			value.Type = static_cast<ecs::PropertyType>(reader.ReadUInt8());
			if (!SerializableValueType(value.Type)) {
				reader.Fail();
				return value;
			}
			switch (value.Type) {
			case ecs::PropertyType::Bool:
				value.Bool = reader.ReadBool();
				break;
			case ecs::PropertyType::Int32:
				value.Int32 = reader.ReadInt32();
				break;
			case ecs::PropertyType::Int64:
				value.Int64 = reader.ReadInt64();
				break;
			case ecs::PropertyType::Float:
				value.Float = reader.ReadFloat();
				break;
			case ecs::PropertyType::Double:
				value.Double = reader.ReadDouble();
				break;
			case ecs::PropertyType::Name:
			case ecs::PropertyType::Enum:
				if (const std::string_view text = reader.ReadString();
					WithinBudget(reader, start) && ValidText(text)) {
					value.Name = core::Name(text);
				} else {
					reader.Fail();
				}
				break;
			case ecs::PropertyType::String:
				if (const std::string_view text = reader.ReadString();
					WithinBudget(reader, start) && text.size() <= VirtualCollection::MAXIMUM_TEXT_BYTES) {
					value.String = std::string(text);
				} else {
					reader.Fail();
				}
				break;
			case ecs::PropertyType::Vector3:
				value.Vector3.X = reader.ReadFloat();
				value.Vector3.Y = reader.ReadFloat();
				value.Vector3.Z = reader.ReadFloat();
				break;
			case ecs::PropertyType::CFrame:
				value.CFrame.Position.X = reader.ReadFloat();
				value.CFrame.Position.Y = reader.ReadFloat();
				value.CFrame.Position.Z = reader.ReadFloat();
				value.CFrame.QuaternionX = reader.ReadFloat();
				value.CFrame.QuaternionY = reader.ReadFloat();
				value.CFrame.QuaternionZ = reader.ReadFloat();
				value.CFrame.QuaternionW = reader.ReadFloat();
				break;
			case ecs::PropertyType::Color3:
				value.Color3.R = reader.ReadFloat();
				value.Color3.G = reader.ReadFloat();
				value.Color3.B = reader.ReadFloat();
				break;
			case ecs::PropertyType::Vector2:
				value.Vector2.X = reader.ReadFloat();
				value.Vector2.Y = reader.ReadFloat();
				break;
			case ecs::PropertyType::UDim:
				value.UDim.Scale = reader.ReadFloat();
				value.UDim.Offset = reader.ReadFloat();
				break;
			case ecs::PropertyType::UDim2:
				value.UDim2.X.Scale = reader.ReadFloat();
				value.UDim2.X.Offset = reader.ReadFloat();
				value.UDim2.Y.Scale = reader.ReadFloat();
				value.UDim2.Y.Offset = reader.ReadFloat();
				break;
			case ecs::PropertyType::Rect:
				value.Rect.Min.X = reader.ReadFloat();
				value.Rect.Min.Y = reader.ReadFloat();
				value.Rect.Max.X = reader.ReadFloat();
				value.Rect.Max.Y = reader.ReadFloat();
				break;
			case ecs::PropertyType::NumberRange:
				value.NumberRange = core::NumberRange{reader.ReadFloat(), reader.ReadFloat()};
				break;
			case ecs::PropertyType::NumberSequence: {
				const uint32_t count = reader.ReadUInt32();
				if (count > core::SEQUENCE_CAPACITY) {
					reader.Fail();
					return value;
				}
				for (uint32_t index = 0; index < count; index++) {
					(void)value.NumberSequence.Add(
						core::NumberKeypoint{reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()}
					);
				}
				break;
			}
			case ecs::PropertyType::ColorSequence: {
				const uint32_t count = reader.ReadUInt32();
				if (count > core::SEQUENCE_CAPACITY) {
					reader.Fail();
					return value;
				}
				for (uint32_t index = 0; index < count; index++) {
					(void)value.ColorSequence.Add(
						core::ColorKeypoint{
							reader.ReadFloat(),
							core::Color3{reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()},
						}
					);
				}
				break;
			}
			case ecs::PropertyType::Reference:
			case ecs::PropertyType::Opaque:
				break;
			}
			return value;
		}

		void WriteVirtualCollections(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *collections = static_cast<const VirtualCollection *>(source);
			for (size_t index = 0; index < count; index++) {
				if (!WriteVirtualCollection(writer, collections[index])) {
					throw std::invalid_argument("invalid virtual collection");
				}
			}
		}

		void ReadVirtualCollections(core::ByteReader &reader, void *destination, size_t count) {
			auto *collections = static_cast<VirtualCollection *>(destination);
			for (size_t index = 0; index < count; index++) {
				if (!ReadVirtualCollection(reader, collections[index])) {
					return;
				}
			}
		}
	}

	const VirtualRecord *FindVirtualRecord(const VirtualCollection &collection, std::string_view key) {
		for (const VirtualRecord &record : collection.Page.Records) {
			if (record.Key == key) {
				return &record;
			}
		}
		return nullptr;
	}

	const VirtualRecord *VirtualRecordAt(const VirtualCollection &collection, uint32_t index) {
		if (index < collection.Page.First) {
			return nullptr;
		}
		const uint32_t offset = index - collection.Page.First;
		return offset < collection.Page.Records.size() ? &collection.Page.Records[offset] : nullptr;
	}

	const ecs::AttributeValue *FindVirtualField(const VirtualRecord &record, std::string_view name) {
		for (const VirtualField &field : record.Fields) {
			if (field.Name == name) {
				return &field.Value;
			}
		}
		return nullptr;
	}

	float VirtualExtentOf(const VirtualCollection &collection, const VirtualRecord &record) {
		return collection.ExtentPolicy == VirtualExtentPolicy::Fixed ? collection.FixedExtent
																	 : record.MeasuredExtent;
	}

	float VirtualExtentBefore(const VirtualCollection &collection, uint32_t index) {
		if (collection.LayoutPolicy == VirtualLayoutPolicy::Grid) {
			if (collection.GridColumns == 0) return 0.0f;
			return static_cast<float>(index / collection.GridColumns) * collection.GridCellStride.Y;
		}
		if (collection.ExtentPolicy == VirtualExtentPolicy::Fixed) {
			return static_cast<float>(index) * collection.FixedExtent;
		}
		if (index < collection.Page.First || index > collection.Page.First + collection.Page.Records.size()) {
			return static_cast<float>(index) * collection.FixedExtent;
		}
		float extent = collection.Page.ExtentBefore;
		for (uint32_t item = collection.Page.First; item < index; item++) {
			extent += collection.Page.Records[item - collection.Page.First].MeasuredExtent;
		}
		return extent;
	}

	float VirtualEstimatedTotalExtent(const VirtualCollection &collection) {
		if (collection.LayoutPolicy == VirtualLayoutPolicy::Grid) {
			if (collection.GridColumns == 0) return 0.0f;
			const uint64_t rows = (static_cast<uint64_t>(collection.ItemCount) + collection.GridColumns - 1) /
								  collection.GridColumns;
			return static_cast<float>(rows) * collection.GridCellStride.Y;
		}
		if (collection.ExtentPolicy == VirtualExtentPolicy::Fixed) {
			return static_cast<float>(collection.ItemCount) * collection.FixedExtent;
		}
		double extent = collection.Page.ExtentBefore;
		for (const VirtualRecord &record : collection.Page.Records) {
			extent += record.MeasuredExtent;
		}
		const uint64_t afterPage =
			static_cast<uint64_t>(collection.Page.First) + collection.Page.Records.size();
		extent += static_cast<double>(collection.ItemCount - afterPage) * collection.FixedExtent;
		return static_cast<float>(extent);
	}

	core::Vector2 VirtualCanvasExtent(const VirtualCollection &collection) {
		if (collection.LayoutPolicy == VirtualLayoutPolicy::Grid) {
			if (collection.GridColumns == 0) return core::Vector2::Zero;
			return core::Vector2{
				static_cast<float>(collection.GridColumns) * collection.GridCellStride.X,
				VirtualEstimatedTotalExtent(collection),
			};
		}
		return core::Vector2{0.0f, VirtualEstimatedTotalExtent(collection)};
	}

	core::Vector2 VirtualCellPosition(const VirtualCollection &collection, uint32_t index) {
		if (collection.LayoutPolicy == VirtualLayoutPolicy::Grid) {
			if (collection.GridColumns == 0) return core::Vector2::Zero;
			return core::Vector2{
				static_cast<float>(index % collection.GridColumns) * collection.GridCellStride.X,
				static_cast<float>(index / collection.GridColumns) * collection.GridCellStride.Y,
			};
		}
		return core::Vector2{0.0f, VirtualExtentBefore(collection, index)};
	}

	bool ValidateVirtualCollection(const VirtualCollection &collection) {
		if (collection.Page.Records.size() > VirtualCollection::MAXIMUM_PAGE_RECORDS ||
			(collection.ExtentPolicy != VirtualExtentPolicy::Fixed &&
			 collection.ExtentPolicy != VirtualExtentPolicy::Measured) ||
			(collection.LayoutPolicy != VirtualLayoutPolicy::List &&
			 collection.LayoutPolicy != VirtualLayoutPolicy::Grid) ||
			(collection.LayoutPolicy == VirtualLayoutPolicy::Grid &&
			 (collection.ExtentPolicy != VirtualExtentPolicy::Fixed || collection.GridColumns == 0 ||
			  !std::isfinite(collection.GridCellStride.X) || !std::isfinite(collection.GridCellStride.Y) ||
			  collection.GridCellStride.X <= 0.0f || collection.GridCellStride.Y <= 0.0f)) ||
			!ValidExtent(collection.FixedExtent) || !std::isfinite(collection.Page.ExtentBefore) ||
			collection.Page.ExtentBefore < 0.0f || collection.Page.First > collection.ItemCount ||
			collection.Page.Records.size() > collection.ItemCount - collection.Page.First ||
			collection.Overscan > VirtualCollection::MAXIMUM_OVERSCAN) {
			return false;
		}

		std::unordered_set<std::string_view> keys;
		keys.reserve(collection.Page.Records.size());
		for (const VirtualRecord &record : collection.Page.Records) {
			if (!ValidText(record.Key) ||
				record.Fields.size() > VirtualCollection::MAXIMUM_FIELDS_PER_RECORD ||
				(collection.ExtentPolicy == VirtualExtentPolicy::Measured &&
				 !ValidExtent(record.MeasuredExtent))) {
				return false;
			}
			if (!keys.insert(record.Key).second) {
				return false;
			}

			std::unordered_set<std::string_view> names;
			names.reserve(record.Fields.size());
			for (const VirtualField &field : record.Fields) {
				if (!ValidText(field.Name) || !ValidValue(field.Value) || !names.insert(field.Name).second) {
					return false;
				}
			}
		}
		const core::Vector2 canvasExtent = VirtualCanvasExtent(collection);
		return std::isfinite(canvasExtent.X) && std::isfinite(canvasExtent.Y) &&
			   canvasExtent.X <= VirtualCollection::MAXIMUM_CANVAS_EXTENT &&
			   canvasExtent.Y <= VirtualCollection::MAXIMUM_CANVAS_EXTENT && MeasureCollection(collection);
	}

	bool WriteVirtualCollection(core::ByteWriter &writer, const VirtualCollection &collection) {
		if (!ValidateVirtualCollection(collection)) {
			return false;
		}

		writer.WriteUInt8(static_cast<uint8_t>(collection.ExtentPolicy));
		writer.WriteFloat(collection.FixedExtent);
		writer.WriteUInt8(static_cast<uint8_t>(collection.LayoutPolicy));
		writer.WriteUInt32(collection.GridColumns);
		writer.WriteFloat(collection.GridCellStride.X);
		writer.WriteFloat(collection.GridCellStride.Y);
		writer.WriteUInt32(collection.Overscan);
		writer.WriteUInt64(collection.Revision);
		writer.WriteUInt32(collection.ItemCount);
		writer.WriteUInt32(collection.Page.First);
		writer.WriteFloat(collection.Page.ExtentBefore);
		writer.WriteUInt32(static_cast<uint32_t>(collection.Page.Records.size()));
		for (const VirtualRecord &record : collection.Page.Records) {
			writer.WriteString(record.Key);
			writer.WriteFloat(record.MeasuredExtent);
			writer.WriteUInt32(static_cast<uint32_t>(record.Fields.size()));
			for (const VirtualField &field : record.Fields) {
				writer.WriteString(field.Name);
				WriteValue(writer, field.Value);
			}
		}
		return true;
	}

	bool ReadVirtualCollection(core::ByteReader &reader, VirtualCollection &collection) {
		const size_t start = reader.Position();
		VirtualCollection decoded;
		decoded.ExtentPolicy = static_cast<VirtualExtentPolicy>(reader.ReadUInt8());
		decoded.FixedExtent = reader.ReadFloat();
		decoded.LayoutPolicy = static_cast<VirtualLayoutPolicy>(reader.ReadUInt8());
		decoded.GridColumns = reader.ReadUInt32();
		decoded.GridCellStride.X = reader.ReadFloat();
		decoded.GridCellStride.Y = reader.ReadFloat();
		decoded.Overscan = reader.ReadUInt32();
		decoded.Revision = reader.ReadUInt64();
		decoded.ItemCount = reader.ReadUInt32();
		decoded.Page.First = reader.ReadUInt32();
		decoded.Page.ExtentBefore = reader.ReadFloat();
		const uint32_t records = reader.ReadUInt32();
		if (!WithinBudget(reader, start) || records > VirtualCollection::MAXIMUM_PAGE_RECORDS) {
			reader.Fail();
			return false;
		}
		decoded.Page.Records.reserve(records);
		for (uint32_t record = 0; record < records; record++) {
			VirtualRecord value;
			if (const std::string_view key = reader.ReadString();
				WithinBudget(reader, start) && ValidText(key)) {
				value.Key = std::string(key);
			} else {
				reader.Fail();
				return false;
			}
			value.MeasuredExtent = reader.ReadFloat();
			const uint32_t fields = reader.ReadUInt32();
			if (!WithinBudget(reader, start) || fields > VirtualCollection::MAXIMUM_FIELDS_PER_RECORD) {
				reader.Fail();
				return false;
			}
			value.Fields.reserve(fields);
			for (uint32_t field = 0; field < fields; field++) {
				VirtualField item;
				if (const std::string_view name = reader.ReadString();
					WithinBudget(reader, start) && ValidText(name)) {
					item.Name = std::string(name);
				} else {
					reader.Fail();
					return false;
				}
				item.Value = ReadValue(reader, start);
				if (!WithinBudget(reader, start)) {
					reader.Fail();
					return false;
				}
				value.Fields.push_back(std::move(item));
			}
			decoded.Page.Records.push_back(std::move(value));
		}
		if (reader.Failed() || !ValidateVirtualCollection(decoded)) {
			reader.Fail();
			return false;
		}
		collection = std::move(decoded);
		return true;
	}

	void RegisterVirtualCollectionComponents() {
		ecs::Components::Register<VirtualCollection>(
			"gui.VirtualCollection",
			WriteVirtualCollections,
			ReadVirtualCollections,
			VirtualCollection::MAXIMUM_PAGE_BYTES
		);
		ecs::Components::Register<VirtualFocusState>(
			"gui.VirtualFocusState",
			WriteTransientVirtualState<VirtualFocusState>,
			ReadTransientVirtualState<VirtualFocusState>
		);
		ecs::Components::Register<VirtualAnchorState>(
			"gui.VirtualAnchorState",
			WriteTransientVirtualState<VirtualAnchorState>,
			ReadTransientVirtualState<VirtualAnchorState>
		);
	}
}
