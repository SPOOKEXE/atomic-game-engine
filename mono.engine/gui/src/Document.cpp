#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Animation.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Document.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Style.hpp>
#include <engine/gui/VirtualCollection.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utf8proc.h>
#include <vector>

namespace engine::gui {

	namespace {
		using ecs::AttributeValue;
		using ecs::ClassId;
		using ecs::PropertyDescriptor;
		using ecs::PropertyType;

		void Issue(DocumentReport &report, std::string path, std::string message, uint32_t line = 0) {
			if (report.Issues.size() >= DocumentLimits::HARD_MAXIMUM_ISSUES) {
				report.Truncated = true;
				return;
			}
			report.Issues.push_back({std::move(path), std::move(message), line});
		}

		bool LimitValid(const DocumentLimits &limits) {
			return limits.MaximumNodes <= DocumentLimits::HARD_MAXIMUM_NODES &&
				   limits.MaximumDepth <= DocumentLimits::HARD_MAXIMUM_DEPTH &&
				   limits.MaximumChildren <= DocumentLimits::HARD_MAXIMUM_CHILDREN &&
				   limits.MaximumProperties <= DocumentLimits::HARD_MAXIMUM_PROPERTIES &&
				   limits.MaximumStringBytes <= DocumentLimits::HARD_MAXIMUM_STRING_BYTES &&
				   limits.MaximumExtensions <= DocumentLimits::HARD_MAXIMUM_EXTENSIONS &&
				   limits.MaximumComponents <= DocumentLimits::HARD_MAXIMUM_COMPONENTS &&
				   limits.MaximumComponentBytes <= DocumentLimits::HARD_MAXIMUM_COMPONENT_BYTES &&
				   limits.MaximumThemes <= DocumentLimits::HARD_MAXIMUM_THEMES &&
				   limits.MaximumBinaryBytes <= DocumentLimits::HARD_MAXIMUM_BINARY_BYTES;
		}

		bool TextWithin(std::string_view text, const DocumentLimits &limits) {
			if (text.size() > limits.MaximumStringBytes) return false;
			for (size_t offset = 0; offset < text.size();) {
				utf8proc_int32_t codepoint = 0;
				const utf8proc_ssize_t consumed = utf8proc_iterate(
					reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset),
					static_cast<utf8proc_ssize_t>(text.size() - offset),
					&codepoint
				);
				if (consumed <= 0) return false;
				offset += static_cast<size_t>(consumed);
			}
			return true;
		}

		bool Finite(float value) {
			return std::isfinite(value);
		}

		// Documents list ordinary writable properties, including their defaults.
		// Visual defaults have no authored meaning, so writing them back through
		// the setter must not turn them into direct style overrides.
		bool ExportStyledProperty(
			const ecs::Store &store, ecs::Entity instance, const PropertyDescriptor &descriptor
		) {
			const StyleDirect *direct = store.Get<StyleDirect>(instance);
			const std::string_view name = descriptor.Spelling;
			auto authored = [&](StyleDirectProperty property) {
				return direct != nullptr && direct->Has(property);
			};
			if (name == "BackgroundColor3") {
				const Background *background = store.Get<Background>(instance);
				return authored(StyleDirectProperty::BackgroundColor) ||
					   (background != nullptr && background->Color != Background{}.Color);
			}
			if (name == "BackgroundTransparency") {
				const Background *background = store.Get<Background>(instance);
				return authored(StyleDirectProperty::BackgroundTransparency) ||
					   (background != nullptr && background->Transparency != Background{}.Transparency);
			}
			if (name == "TextColor3") {
				const Label *label = store.Get<Label>(instance);
				return authored(StyleDirectProperty::TextColor) ||
					   (label != nullptr && label->Color != Label{}.Color);
			}
			if (name == "TextTransparency") {
				const Label *label = store.Get<Label>(instance);
				return authored(StyleDirectProperty::TextTransparency) ||
					   (label != nullptr && label->Transparency != Label{}.Transparency);
			}
			if (name == "ImageColor3") {
				const Picture *picture = store.Get<Picture>(instance);
				return authored(StyleDirectProperty::ImageColor) ||
					   (picture != nullptr && picture->Color != Picture{}.Color);
			}
			if (name == "ImageTransparency") {
				const Picture *picture = store.Get<Picture>(instance);
				return authored(StyleDirectProperty::ImageTransparency) ||
					   (picture != nullptr && picture->Transparency != Picture{}.Transparency);
			}
			return true;
		}

		bool FiniteValue(const DocumentProperty &property) {
			const AttributeValue &value = property.Value;
			switch (property.Type) {
			case PropertyType::Float:
				return Finite(value.Float);
			case PropertyType::Double:
				return std::isfinite(value.Double);
			case PropertyType::Vector2:
				return Finite(value.Vector2.X) && Finite(value.Vector2.Y);
			case PropertyType::Vector3:
				return Finite(value.Vector3.X) && Finite(value.Vector3.Y) && Finite(value.Vector3.Z);
			case PropertyType::Color3:
				return Finite(value.Color3.R) && Finite(value.Color3.G) && Finite(value.Color3.B);
			case PropertyType::UDim:
				return Finite(value.UDim.Scale) && Finite(value.UDim.Offset);
			case PropertyType::UDim2:
				return Finite(value.UDim2.X.Scale) && Finite(value.UDim2.X.Offset) &&
					   Finite(value.UDim2.Y.Scale) && Finite(value.UDim2.Y.Offset);
			case PropertyType::Rect:
				return Finite(value.Rect.Min.X) && Finite(value.Rect.Min.Y) && Finite(value.Rect.Max.X) &&
					   Finite(value.Rect.Max.Y);
			case PropertyType::NumberRange:
				return Finite(value.NumberRange.Minimum) && Finite(value.NumberRange.Maximum);
			case PropertyType::CFrame:
				return Finite(value.CFrame.Position.X) && Finite(value.CFrame.Position.Y) &&
					   Finite(value.CFrame.Position.Z) && Finite(value.CFrame.QuaternionX) &&
					   Finite(value.CFrame.QuaternionY) && Finite(value.CFrame.QuaternionZ) &&
					   Finite(value.CFrame.QuaternionW);
			case PropertyType::NumberSequence:
				if (value.NumberSequence.Count > core::SEQUENCE_CAPACITY) return false;
				for (uint32_t index = 0; index < value.NumberSequence.Count; index++) {
					const auto &key = value.NumberSequence.Keypoints[index];
					if (!Finite(key.Time) || !Finite(key.Value) || !Finite(key.Envelope)) return false;
				}
				return true;
			case PropertyType::ColorSequence:
				if (value.ColorSequence.Count > core::SEQUENCE_CAPACITY) return false;
				for (uint32_t index = 0; index < value.ColorSequence.Count; index++) {
					const auto &key = value.ColorSequence.Keypoints[index];
					if (!Finite(key.Time) || !Finite(key.Value.R) || !Finite(key.Value.G) ||
						!Finite(key.Value.B))
						return false;
				}
				return true;
			default:
				return true;
			}
		}

		const PropertyDescriptor *PropertyOf(ClassId id, std::string_view name) {
			for (const PropertyDescriptor &property : ecs::Classes::Describe(id).Properties) {
				if (property.Spelling == name) return &property;
			}
			return nullptr;
		}

		bool DocumentCreatable(ClassId id) {
			if (!id.IsValid()) return false;
			const ClassId guiBase = GuiClass("GuiBase");
			const ClassId uiBase = GuiClass("UIBase");
			const ClassId binding = GuiClass("UIBinding");
			const ClassId virtualCollection = GuiClass("UIVirtualCollection");
			const ecs::ClassInfo &info = ecs::Classes::Describe(id);
			const bool gui = guiBase.IsValid() && ecs::Classes::IsA(id, guiBase);
			const bool modifier = uiBase.IsValid() && ecs::Classes::IsA(id, uiBase);
			const bool special = id == binding || id == virtualCollection;
			return (gui || modifier || special) && info.Creatable && info.Kind == ecs::ClassKind::Concrete;
		}

		bool DocumentRoot(ClassId id) {
			const ClassId guiBase = GuiClass("GuiBase");
			return guiBase.IsValid() && ecs::Classes::IsA(id, guiBase);
		}

		bool DocumentComponentName(std::string_view name) {
			return name == "gui.UIStyle" || name == "gui.AnimationPlayback" ||
				   name == "gui.VirtualCollection" || name == "gui.Binding" || name == "gui.StyleClass" ||
				   name == "gui.StyleDirect" || name == "gui.LabelPresentation" ||
				   name == "gui.LabelLocalizationArguments";
		}

		bool ValidDocumentParent(ClassId child, ClassId parent) {
			const ClassId guiBase = GuiClass("GuiBase");
			const ClassId guiObject = GuiClass("GuiObject");
			const ClassId uiBase = GuiClass("UIBase");
			const ClassId binding = GuiClass("UIBinding");
			const ClassId gradient = GuiClass("UIGradient");
			const ClassId stroke = GuiClass("UIStroke");
			const ClassId scrollingFrame = GuiClass("ScrollingFrame");
			const ClassId virtualCollection = GuiClass("UIVirtualCollection");
			if (child == virtualCollection)
				return scrollingFrame.IsValid() && ecs::Classes::IsA(parent, scrollingFrame);
			if (guiBase.IsValid() && ecs::Classes::IsA(child, guiBase))
				return parent == virtualCollection || ecs::Classes::IsA(parent, guiBase);
			if (child == gradient && parent == stroke) return true;
			if (uiBase.IsValid() && ecs::Classes::IsA(child, uiBase))
				return guiObject.IsValid() && ecs::Classes::IsA(parent, guiObject);
			if (child == binding) return guiObject.IsValid() && ecs::Classes::IsA(parent, guiObject);
			return false;
		}

		const void *PropertyBytes(const DocumentProperty &property) {
			const AttributeValue &value = property.Value;
			switch (property.Type) {
			case PropertyType::Bool:
				return &value.Bool;
			case PropertyType::Int32:
				return &value.Int32;
			case PropertyType::Int64:
				return &value.Int64;
			case PropertyType::Float:
				return &value.Float;
			case PropertyType::Double:
				return &value.Double;
			case PropertyType::Name:
			case PropertyType::Enum:
				return &value.Name;
			case PropertyType::String:
				return &value.String;
			case PropertyType::Vector3:
				return &value.Vector3;
			case PropertyType::CFrame:
				return &value.CFrame;
			case PropertyType::Color3:
				return &value.Color3;
			case PropertyType::Vector2:
				return &value.Vector2;
			case PropertyType::UDim:
				return &value.UDim;
			case PropertyType::UDim2:
				return &value.UDim2;
			case PropertyType::Rect:
				return &value.Rect;
			case PropertyType::NumberRange:
				return &value.NumberRange;
			case PropertyType::NumberSequence:
				return &value.NumberSequence;
			case PropertyType::ColorSequence:
				return &value.ColorSequence;
			case PropertyType::Reference:
			case PropertyType::Opaque:
				return nullptr;
			}
			return nullptr;
		}

		bool CopyProperty(
			const ecs::Store &store,
			ecs::Entity instance,
			const PropertyDescriptor &descriptor,
			DocumentProperty &out
		) {
			out.Type = descriptor.Type;
			out.Value.Type = descriptor.Type;
			switch (descriptor.Type) {
			case PropertyType::Bool:
				return store.GetProperty(instance, descriptor, &out.Value.Bool, sizeof(out.Value.Bool));
			case PropertyType::Int32:
				return store.GetProperty(instance, descriptor, &out.Value.Int32, sizeof(out.Value.Int32));
			case PropertyType::Int64:
				return store.GetProperty(instance, descriptor, &out.Value.Int64, sizeof(out.Value.Int64));
			case PropertyType::Float:
				return store.GetProperty(instance, descriptor, &out.Value.Float, sizeof(out.Value.Float));
			case PropertyType::Double:
				return store.GetProperty(instance, descriptor, &out.Value.Double, sizeof(out.Value.Double));
			case PropertyType::Name:
			case PropertyType::Enum:
				return store.GetProperty(instance, descriptor, &out.Value.Name, sizeof(out.Value.Name));
			case PropertyType::String:
				return store.GetProperty(instance, descriptor, &out.Value.String, sizeof(out.Value.String));
			case PropertyType::Vector3:
				return store.GetProperty(instance, descriptor, &out.Value.Vector3, sizeof(out.Value.Vector3));
			case PropertyType::CFrame:
				return store.GetProperty(instance, descriptor, &out.Value.CFrame, sizeof(out.Value.CFrame));
			case PropertyType::Color3:
				return store.GetProperty(instance, descriptor, &out.Value.Color3, sizeof(out.Value.Color3));
			case PropertyType::Vector2:
				return store.GetProperty(instance, descriptor, &out.Value.Vector2, sizeof(out.Value.Vector2));
			case PropertyType::UDim:
				return store.GetProperty(instance, descriptor, &out.Value.UDim, sizeof(out.Value.UDim));
			case PropertyType::UDim2:
				return store.GetProperty(instance, descriptor, &out.Value.UDim2, sizeof(out.Value.UDim2));
			case PropertyType::Rect:
				return store.GetProperty(instance, descriptor, &out.Value.Rect, sizeof(out.Value.Rect));
			case PropertyType::NumberRange:
				return store.GetProperty(
					instance, descriptor, &out.Value.NumberRange, sizeof(out.Value.NumberRange)
				);
			case PropertyType::NumberSequence:
				return store.GetProperty(
					instance, descriptor, &out.Value.NumberSequence, sizeof(out.Value.NumberSequence)
				);
			case PropertyType::ColorSequence:
				return store.GetProperty(
					instance, descriptor, &out.Value.ColorSequence, sizeof(out.Value.ColorSequence)
				);
			case PropertyType::Reference:
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}

		constexpr uint64_t DOCUMENT_MAGIC = 0x5549'444F'4355'4D45ull;

		bool ValidPropertyType(PropertyType type) {
			switch (type) {
			case PropertyType::Bool:
			case PropertyType::Int32:
			case PropertyType::Int64:
			case PropertyType::Float:
			case PropertyType::Double:
			case PropertyType::Name:
			case PropertyType::Enum:
			case PropertyType::String:
			case PropertyType::Reference:
			case PropertyType::Vector3:
			case PropertyType::CFrame:
			case PropertyType::Color3:
			case PropertyType::Vector2:
			case PropertyType::UDim:
			case PropertyType::UDim2:
			case PropertyType::Rect:
			case PropertyType::NumberRange:
			case PropertyType::NumberSequence:
			case PropertyType::ColorSequence:
				return true;
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}

		void WriteDocumentValue(core::ByteWriter &writer, const DocumentProperty &property) {
			const AttributeValue &value = property.Value;
			switch (property.Type) {
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
			case PropertyType::Reference:
				writer.WriteString(property.Reference);
				break;
			case PropertyType::Vector3:
				writer.WriteFloat(value.Vector3.X);
				writer.WriteFloat(value.Vector3.Y);
				writer.WriteFloat(value.Vector3.Z);
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
			case PropertyType::Color3:
				writer.WriteFloat(value.Color3.R);
				writer.WriteFloat(value.Color3.G);
				writer.WriteFloat(value.Color3.B);
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
			case PropertyType::Opaque:
				break;
			}
		}

		bool ReadText(core::ByteReader &reader, std::string &out, const DocumentLimits &limits) {
			const std::string_view text = reader.ReadString();
			if (reader.Failed() || text.size() > limits.MaximumStringBytes) {
				reader.Fail();
				return false;
			}
			out.assign(text);
			return true;
		}

		bool ReadDocumentValue(
			core::ByteReader &reader, DocumentProperty &property, const DocumentLimits &limits
		) {
			AttributeValue &value = property.Value;
			value = {};
			value.Type = property.Type;
			switch (property.Type) {
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
			case PropertyType::Enum: {
				std::string text;
				if (!ReadText(reader, text, limits)) return false;
				value.Name = core::Name(text);
				break;
			}
			case PropertyType::String:
				if (!ReadText(reader, value.String, limits)) return false;
				break;
			case PropertyType::Reference:
				if (!ReadText(reader, property.Reference, limits)) return false;
				break;
			case PropertyType::Vector3:
				value.Vector3 = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				break;
			case PropertyType::CFrame:
				value.CFrame.Position = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				value.CFrame.QuaternionX = reader.ReadFloat();
				value.CFrame.QuaternionY = reader.ReadFloat();
				value.CFrame.QuaternionZ = reader.ReadFloat();
				value.CFrame.QuaternionW = reader.ReadFloat();
				break;
			case PropertyType::Color3:
				value.Color3 = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				break;
			case PropertyType::Vector2:
				value.Vector2 = {reader.ReadFloat(), reader.ReadFloat()};
				break;
			case PropertyType::UDim:
				value.UDim = {reader.ReadFloat(), reader.ReadFloat()};
				break;
			case PropertyType::UDim2:
				value.UDim2.X = {reader.ReadFloat(), reader.ReadFloat()};
				value.UDim2.Y = {reader.ReadFloat(), reader.ReadFloat()};
				break;
			case PropertyType::Rect:
				value.Rect.Min = {reader.ReadFloat(), reader.ReadFloat()};
				value.Rect.Max = {reader.ReadFloat(), reader.ReadFloat()};
				break;
			case PropertyType::NumberRange:
				value.NumberRange = {reader.ReadFloat(), reader.ReadFloat()};
				break;
			case PropertyType::NumberSequence: {
				const uint32_t count = reader.ReadUInt32();
				if (count > core::SEQUENCE_CAPACITY) {
					reader.Fail();
					return false;
				}
				for (uint32_t index = 0; index < count; index++)
					(void)value.NumberSequence.Add(
						{reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()}
					);
				break;
			}
			case PropertyType::ColorSequence: {
				const uint32_t count = reader.ReadUInt32();
				if (count > core::SEQUENCE_CAPACITY) {
					reader.Fail();
					return false;
				}
				for (uint32_t index = 0; index < count; index++)
					(void)value.ColorSequence.Add(
						{reader.ReadFloat(), {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()}}
					);
				break;
			}
			case PropertyType::Opaque:
				reader.Fail();
				return false;
			}
			return !reader.Failed();
		}

		void WriteStyleTokens(core::ByteWriter &writer, const StyleSet &tokens) {
			writer.WriteUInt32(static_cast<uint32_t>(tokens.Declarations().size()));
			for (const StyleDeclaration &declaration : tokens.Declarations()) {
				writer.WriteName(declaration.Name);
				writer.WriteUInt8(static_cast<uint8_t>(declaration.Value.Type));
				if (declaration.Value.Type == StyleValueType::Color) {
					writer.WriteFloat(declaration.Value.Color.R);
					writer.WriteFloat(declaration.Value.Color.G);
					writer.WriteFloat(declaration.Value.Color.B);
				} else {
					writer.WriteFloat(declaration.Value.Number);
				}
			}
		}

		bool ReadStyleTokens(core::ByteReader &reader, StyleSet &tokens, const DocumentLimits &limits) {
			tokens = {};
			const uint32_t count = reader.ReadUInt32();
			if (count > StyleSet::MAXIMUM_DECLARATIONS) {
				reader.Fail();
				return false;
			}
			for (uint32_t index = 0; index < count; index++) {
				std::string name;
				if (!ReadText(reader, name, limits) || name.empty()) return false;
				StyleDeclaration declaration;
				declaration.Name = core::Name(name);
				declaration.Value.Type = static_cast<StyleValueType>(reader.ReadUInt8());
				if (declaration.Value.Type == StyleValueType::Color) {
					declaration.Value.Color = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				} else if (declaration.Value.Type == StyleValueType::Number) {
					declaration.Value.Number = reader.ReadFloat();
				} else {
					reader.Fail();
					return false;
				}
				if (!tokens.Set(declaration)) {
					reader.Fail();
					return false;
				}
			}
			return !reader.Failed();
		}

		void WriteNode(core::ByteWriter &writer, const DocumentNode &node) {
			writer.WriteString(node.Id);
			writer.WriteString(node.Class);
			writer.WriteString(node.Name);
			writer.WriteUInt32(static_cast<uint32_t>(node.Properties.size()));
			for (const DocumentProperty &property : node.Properties) {
				writer.WriteString(property.Name);
				writer.WriteUInt8(static_cast<uint8_t>(property.Type));
				WriteDocumentValue(writer, property);
				writer.WriteUInt32(property.SourceLine);
			}
			writer.WriteUInt32(static_cast<uint32_t>(node.Extensions.size()));
			for (const DocumentExtension &extension : node.Extensions) {
				writer.WriteString(extension.Name);
				writer.WriteString(extension.Payload);
				writer.WriteUInt32(extension.SourceLine);
			}
			writer.WriteUInt32(static_cast<uint32_t>(node.Components.size()));
			for (const DocumentComponent &component : node.Components) {
				writer.WriteString(component.Name);
				writer.WriteUInt32(static_cast<uint32_t>(component.Bytes.size()));
				writer.WriteRaw(component.Bytes.data(), component.Bytes.size());
			}
			writer.WriteUInt32(static_cast<uint32_t>(node.Children.size()));
			for (const DocumentNode &child : node.Children)
				WriteNode(writer, child);
		}

		bool ReadNode(
			core::ByteReader &reader,
			DocumentNode &node,
			uint32_t depth,
			uint32_t &nodes,
			const DocumentLimits &limits
		) {
			if (depth > limits.MaximumDepth || ++nodes > limits.MaximumNodes) {
				reader.Fail();
				return false;
			}
			if (!ReadText(reader, node.Id, limits) || !ReadText(reader, node.Class, limits) ||
				!ReadText(reader, node.Name, limits))
				return false;
			const uint32_t properties = reader.ReadUInt32();
			if (properties > limits.MaximumProperties) {
				reader.Fail();
				return false;
			}
			node.Properties.reserve(properties);
			for (uint32_t index = 0; index < properties; index++) {
				DocumentProperty property;
				if (!ReadText(reader, property.Name, limits)) return false;
				property.Type = static_cast<PropertyType>(reader.ReadUInt8());
				if (!ValidPropertyType(property.Type) || !ReadDocumentValue(reader, property, limits))
					return false;
				property.SourceLine = reader.ReadUInt32();
				node.Properties.push_back(std::move(property));
			}
			const uint32_t extensions = reader.ReadUInt32();
			if (extensions > limits.MaximumExtensions) {
				reader.Fail();
				return false;
			}
			node.Extensions.reserve(extensions);
			for (uint32_t index = 0; index < extensions; index++) {
				DocumentExtension extension;
				if (!ReadText(reader, extension.Name, limits) || !ReadText(reader, extension.Payload, limits))
					return false;
				extension.SourceLine = reader.ReadUInt32();
				node.Extensions.push_back(std::move(extension));
			}
			const uint32_t components = reader.ReadUInt32();
			if (components > limits.MaximumComponents) {
				reader.Fail();
				return false;
			}
			node.Components.reserve(components);
			for (uint32_t index = 0; index < components; index++) {
				DocumentComponent component;
				if (!ReadText(reader, component.Name, limits)) return false;
				const uint32_t bytes = reader.ReadUInt32();
				if (bytes > limits.MaximumComponentBytes || bytes > reader.Remaining()) {
					reader.Fail();
					return false;
				}
				const std::span<const std::byte> payload = reader.ReadRawView(bytes);
				component.Bytes.assign(payload.begin(), payload.end());
				node.Components.push_back(std::move(component));
			}
			const uint32_t children = reader.ReadUInt32();
			if (children > limits.MaximumChildren) {
				reader.Fail();
				return false;
			}
			node.Children.reserve(children);
			for (uint32_t index = 0; index < children; index++) {
				node.Children.emplace_back();
				if (!ReadNode(reader, node.Children.back(), depth + 1, nodes, limits)) return false;
			}
			return !reader.Failed();
		}
	}

	bool ValidateDocument(const UiDocument &document, DocumentReport &report, const DocumentLimits &limits) {
		report.Issues.clear();
		report.Truncated = false;
		if (!LimitValid(limits)) {
			Issue(report, "$", "document limits exceed engine ceilings");
			return false;
		}
		RegisterGuiClasses();
		if (document.Version != UI_DOCUMENT_VERSION) {
			Issue(report, "$", "unsupported UI document version");
			return false;
		}
		if (document.Themes.size() > limits.MaximumThemes) {
			Issue(report, "$.themes", "theme limit exceeded");
			return false;
		}
		std::unordered_set<std::string> ids;
		std::unordered_set<std::string> themeIds;
		for (size_t index = 0; index < document.Themes.size(); index++) {
			const DocumentTheme &theme = document.Themes[index];
			const std::string path = "$.themes[" + std::to_string(index) + "]";
			if (theme.Id.empty() || !themeIds.insert(theme.Id).second || !TextWithin(theme.Id, limits) ||
				!TextWithin(theme.Name, limits))
				Issue(report, path, "theme id is empty, duplicated, or exceeds string limits");
			if (theme.Tokens.Declarations().size() > StyleSet::MAXIMUM_DECLARATIONS)
				Issue(report, path, "theme token limit exceeded");
			for (const StyleDeclaration &token : theme.Tokens.Declarations()) {
				if (token.Name.Text().empty() || !TextWithin(token.Name.Text(), limits) ||
					(token.Value.Type == StyleValueType::Color &&
					 (!Finite(token.Value.Color.R) || !Finite(token.Value.Color.G) ||
					  !Finite(token.Value.Color.B))) ||
					(token.Value.Type == StyleValueType::Number && !Finite(token.Value.Number)))
					Issue(report, path, "theme contains an empty token name or non-finite value");
			}
		}
		uint32_t nodes = 0;
		std::function<void(const DocumentNode &, std::string, uint32_t, ClassId)> validate;
		validate = [&](const DocumentNode &node, std::string path, uint32_t depth, ClassId parent) {
			if (report.Truncated) return;
			if (++nodes > limits.MaximumNodes) {
				Issue(report, path, "node limit exceeded");
				return;
			}
			if (depth > limits.MaximumDepth) {
				Issue(report, path, "hierarchy depth limit exceeded");
				return;
			}
			if (node.Children.size() > limits.MaximumChildren) {
				Issue(report, path, "child limit exceeded");
				return;
			}
			if (!TextWithin(node.Id, limits) || !TextWithin(node.Class, limits) ||
				!TextWithin(node.Name, limits))
				Issue(report, path, "node string limit exceeded");
			if (node.Id.empty() || !ids.insert(node.Id).second)
				Issue(report, path, "node id is empty or duplicated");
			const ClassId id = GuiClass(node.Class);
			if (!DocumentCreatable(id))
				Issue(report, path, "class is unknown, abstract, or not a GUI document class");
			else if (!parent.IsValid() && !DocumentRoot(id))
				Issue(report, path, "class cannot be a document root");
			else if (parent.IsValid() && !ValidDocumentParent(id, parent))
				Issue(report, path, "class cannot be a child of this document class");
			if (node.Properties.size() > limits.MaximumProperties)
				Issue(report, path, "property limit exceeded");
			if (node.Extensions.size() > limits.MaximumExtensions)
				Issue(report, path, "extension limit exceeded");
			if (node.Components.size() > limits.MaximumComponents)
				Issue(report, path, "authored component limit exceeded");
			std::unordered_set<std::string> properties;
			for (const DocumentProperty &property : node.Properties) {
				if (report.Truncated) return;
				const std::string propertyPath = path + "." + property.Name;
				if (!TextWithin(property.Name, limits) || !TextWithin(property.Reference, limits) ||
					!TextWithin(property.Value.String, limits) ||
					!TextWithin(property.Value.Name.Text(), limits))
					Issue(report, propertyPath, "property string limit exceeded", property.SourceLine);
				if (!properties.insert(property.Name).second)
					Issue(report, propertyPath, "property is duplicated", property.SourceLine);
				const PropertyDescriptor *descriptor = PropertyOf(id, property.Name);
				if (property.Name == "Name" || property.Name == "Parent" || descriptor == nullptr ||
					!descriptor->Writable) {
					Issue(report, propertyPath, "property is unknown or not importable", property.SourceLine);
				} else if (descriptor->Type != property.Type || property.Value.Type != property.Type) {
					Issue(
						report,
						propertyPath,
						"property type does not match class surface",
						property.SourceLine
					);
				} else if (!FiniteValue(property)) {
					Issue(
						report,
						propertyPath,
						"property contains a non-finite or malformed value",
						property.SourceLine
					);
				}
			}
			for (const DocumentExtension &extension : node.Extensions) {
				if (report.Truncated) return;
				if (!TextWithin(extension.Name, limits) || !TextWithin(extension.Payload, limits))
					Issue(
						report, path + ".extensions", "extension string limit exceeded", extension.SourceLine
					);
			}
			std::unordered_set<std::string> componentNames;
			for (const DocumentComponent &component : node.Components) {
				if (!TextWithin(component.Name, limits) || !DocumentComponentName(component.Name) ||
					!componentNames.insert(component.Name).second ||
					component.Bytes.size() > limits.MaximumComponentBytes) {
					Issue(report, path, "authored component is invalid or exceeds limits");
				}
			}
			for (size_t index = 0; index < node.Children.size() && !report.Truncated; index++)
				validate(
					node.Children[index], path + ".children[" + std::to_string(index) + "]", depth + 1, id
				);
		};
		for (size_t index = 0; index < document.Roots.size() && !report.Truncated; index++)
			validate(document.Roots[index], "$.roots[" + std::to_string(index) + "]", 1, {});
		if (!report.Ok()) return false;
		for (const std::string &themeId : themeIds) {
			if (ids.contains(themeId)) {
				Issue(report, "$.themes", "theme id duplicates a node id");
				return false;
			}
		}
		std::function<void(const DocumentNode &, std::string)> references;
		references = [&](const DocumentNode &node, std::string path) {
			if (report.Truncated) return;
			for (const DocumentProperty &property : node.Properties) {
				if (report.Truncated) return;
				if (property.Type != PropertyType::Reference || property.Reference.empty()) continue;
				const bool theme = property.Name == "Theme";
				if ((theme && !themeIds.contains(property.Reference)) ||
					(!theme && !ids.contains(property.Reference)))
					Issue(
						report,
						path + "." + property.Name,
						theme ? "theme reference names no theme in this document"
							  : "reference names no node in this document",
						property.SourceLine
					);
			}
			for (size_t index = 0; index < node.Children.size() && !report.Truncated; index++)
				references(node.Children[index], path + ".children[" + std::to_string(index) + "]");
		};
		for (size_t index = 0; index < document.Roots.size() && !report.Truncated; index++)
			references(document.Roots[index], "$.roots[" + std::to_string(index) + "]");
		return report.Ok();
	}

	bool EncodeDocument(
		const UiDocument &document,
		core::ByteWriter &writer,
		DocumentReport &report,
		const DocumentLimits &limits
	) {
		if (!ValidateDocument(document, report, limits)) return false;
		try {
			core::ByteWriter encoded(0, limits.MaximumBinaryBytes);
			encoded.WriteUInt64(DOCUMENT_MAGIC);
			encoded.WriteUInt32(document.Version);
			encoded.WriteUInt32(static_cast<uint32_t>(document.Themes.size()));
			for (const DocumentTheme &theme : document.Themes) {
				encoded.WriteString(theme.Id);
				encoded.WriteString(theme.Name);
				WriteStyleTokens(encoded, theme.Tokens);
			}
			encoded.WriteUInt32(static_cast<uint32_t>(document.Roots.size()));
			for (const DocumentNode &root : document.Roots)
				WriteNode(encoded, root);
			if (writer.Remaining() < encoded.Size()) {
				Issue(report, "$", "destination writer has insufficient capacity");
				return false;
			}
			writer.WriteRaw(encoded.Bytes().data(), encoded.Size());
			return true;
		} catch (const std::length_error &) {
			Issue(report, "$", "document exceeds binary size limit");
			return false;
		}
	}

	bool DecodeDocument(
		core::ByteReader &reader, UiDocument &document, DocumentReport &report, const DocumentLimits &limits
	) {
		report.Issues.clear();
		report.Truncated = false;
		if (!LimitValid(limits)) {
			Issue(report, "$", "document limits exceed engine ceilings");
			return false;
		}
		if (reader.Remaining() > limits.MaximumBinaryBytes) {
			reader.Fail();
			Issue(report, "$", "document binary size limit exceeded");
			return false;
		}
		UiDocument decoded;
		if (reader.ReadUInt64() != DOCUMENT_MAGIC) {
			reader.Fail();
			Issue(report, "$", "document binary header is invalid");
			return false;
		}
		decoded.Version = reader.ReadUInt32();
		const uint32_t themes = reader.ReadUInt32();
		if (themes > limits.MaximumThemes) {
			reader.Fail();
			Issue(report, "$.themes", "theme limit exceeded");
			return false;
		}
		decoded.Themes.reserve(themes);
		for (uint32_t index = 0; index < themes; index++) {
			DocumentTheme theme;
			if (!ReadText(reader, theme.Id, limits) || !ReadText(reader, theme.Name, limits) ||
				!ReadStyleTokens(reader, theme.Tokens, limits)) {
				Issue(report, "$.themes[" + std::to_string(index) + "]", "malformed theme");
				return false;
			}
			decoded.Themes.push_back(std::move(theme));
		}
		const uint32_t roots = reader.ReadUInt32();
		if (roots > limits.MaximumNodes) {
			reader.Fail();
			Issue(report, "$.roots", "node limit exceeded");
			return false;
		}
		decoded.Roots.reserve(roots);
		uint32_t nodes = 0;
		for (uint32_t index = 0; index < roots; index++) {
			decoded.Roots.emplace_back();
			if (!ReadNode(reader, decoded.Roots.back(), 1, nodes, limits)) {
				Issue(report, "$.roots[" + std::to_string(index) + "]", "malformed node");
				return false;
			}
		}
		if (!reader.AtEnd()) {
			reader.Fail();
			Issue(report, "$", "document binary is truncated or has trailing bytes");
			return false;
		}
		if (!ValidateDocument(decoded, report, limits)) return false;
		document = std::move(decoded);
		return true;
	}

	bool ImportDocument(
		ecs::Store &store,
		const UiDocument &document,
		std::vector<ecs::Entity> &roots,
		DocumentReport &report,
		const DocumentLimits &limits,
		std::vector<ecs::Entity> *themesOut
	) {
		if (!ValidateDocument(document, report, limits)) return false;
		if (store.AdoptOnly()) {
			Issue(report, "$", "store refuses authoritative document instances");
			return false;
		}
		struct PlannedNode {
			const DocumentNode *Source = nullptr;
			ClassId Class;
			size_t Parent = 0;
			std::string Path;
			ecs::Entity Instance = ecs::NULL_ENTITY;
		};
		struct PlannedProperty {
			const DocumentProperty *Source = nullptr;
			const PropertyDescriptor *Descriptor = nullptr;
			size_t Node = 0;
			std::string Path;
		};
		struct PlannedComponent {
			const DocumentComponent *Source = nullptr;
			size_t Node = 0;
			std::string Path;
		};
		struct PlannedTheme {
			const DocumentTheme *Source = nullptr;
			ecs::Entity Instance = ecs::NULL_ENTITY;
		};

		constexpr size_t NO_PARENT = std::numeric_limits<size_t>::max();
		std::vector<PlannedNode> nodes;
		std::vector<PlannedTheme> themes;
		std::vector<PlannedProperty> properties;
		std::vector<PlannedComponent> components;
		std::unordered_map<std::string_view, size_t> indices;
		std::unordered_map<std::string_view, size_t> themeIndices;
		for (size_t index = 0; index < document.Themes.size(); index++) {
			themes.push_back({&document.Themes[index]});
			themeIndices.emplace(document.Themes[index].Id, index);
		}
		std::function<void(const DocumentNode &, size_t, std::string)> stage;
		stage = [&](const DocumentNode &node, size_t parent, std::string path) {
			const size_t index = nodes.size();
			nodes.push_back({&node, GuiClass(node.Class), parent, std::move(path)});
			indices.emplace(node.Id, index);
			for (const DocumentProperty &property : node.Properties) {
				const PropertyDescriptor *descriptor = PropertyOf(nodes[index].Class, property.Name);
				if (descriptor->PrepareDocument != nullptr) {
					Issue(
						report,
						nodes[index].Path + "." + property.Name,
						"property requires an unsupported document prerequisite",
						property.SourceLine
					);
					return;
				}
				properties.push_back({&property, descriptor, index, nodes[index].Path + "." + property.Name});
			}
			for (const DocumentComponent &component : node.Components) {
				components.push_back(
					{&component, index, nodes[index].Path + ".components." + component.Name}
				);
			}
			for (size_t child = 0; child < node.Children.size() && report.Ok(); child++)
				stage(
					node.Children[child],
					index,
					nodes[index].Path + ".children[" + std::to_string(child) + "]"
				);
		};
		for (size_t root = 0; root < document.Roots.size() && report.Ok(); root++)
			stage(document.Roots[root], NO_PARENT, "$.roots[" + std::to_string(root) + "]");
		if (!report.Ok()) return false;

		auto rollback = [&]() {
			for (auto node = nodes.rbegin(); node != nodes.rend(); ++node) {
				if (store.Alive(node->Instance)) store.DestroyInstance(node->Instance);
			}
			for (auto theme = themes.rbegin(); theme != themes.rend(); ++theme) {
				if (store.Alive(theme->Instance)) store.DestroyInstance(theme->Instance);
			}
		};
		for (PlannedTheme &theme : themes) {
			theme.Instance = store.CreateInstance(GuiClass("UITheme"), theme.Source->Name);
			if (theme.Instance == ecs::NULL_ENTITY) {
				Issue(report, "$.themes", "store refused document theme");
				rollback();
				return false;
			}
			store.Set(theme.Instance, UITheme{theme.Source->Tokens});
		}
		for (PlannedNode &node : nodes) {
			node.Instance = store.CreateInstance(node.Class, node.Source->Name);
			if (node.Instance == ecs::NULL_ENTITY) {
				Issue(report, node.Path, "store refused to create GUI instance");
				rollback();
				return false;
			}
		}
		for (const PlannedProperty &property : properties) {
			if (property.Source->Type == PropertyType::Reference) continue;
			const void *value = PropertyBytes(*property.Source);
			if (value == nullptr ||
				!store.SetProperty(
					nodes[property.Node].Instance, *property.Descriptor, value, property.Descriptor->Size
				)) {
				Issue(report, property.Path, "store refused document property", property.Source->SourceLine);
				rollback();
				return false;
			}
		}
		auto componentDestination = [&](ecs::Entity instance, std::string_view name) -> void * {
			if (name == "gui.UIStyle") return store.GetMutable<UIStyle>(instance);
			if (name == "gui.AnimationPlayback") return store.GetMutable<AnimationPlayback>(instance);
			if (name == "gui.VirtualCollection") return store.GetMutable<VirtualCollection>(instance);
			if (name == "gui.Binding") return store.GetMutable<Binding>(instance);
			if (name == "gui.StyleClass") return store.GetMutable<StyleClass>(instance);
			if (name == "gui.StyleDirect") return store.GetMutable<StyleDirect>(instance);
			if (name == "gui.LabelPresentation") return store.GetMutable<LabelPresentation>(instance);
			if (name == "gui.LabelLocalizationArguments")
				return store.GetMutable<LabelLocalizationArguments>(instance);
			return nullptr;
		};
		for (const PlannedComponent &component : components) {
			void *destination = componentDestination(nodes[component.Node].Instance, component.Source->Name);
			const ecs::ComponentId id = ecs::Components::Find(core::Name(component.Source->Name));
			const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);
			if (destination == nullptr || !descriptor.Serialisable || descriptor.Write == nullptr ||
				descriptor.Read == nullptr) {
				Issue(report, component.Path, "component is not an authored document component");
				rollback();
				return false;
			}
			core::ByteReader payload(component.Source->Bytes);
			descriptor.Read(payload, destination, 1);
			if (!payload.AtEnd()) {
				Issue(report, component.Path, "component payload is malformed");
				rollback();
				return false;
			}
		}
		for (const PlannedNode &node : nodes) {
			if (node.Parent != NO_PARENT && !store.SetParent(node.Instance, nodes[node.Parent].Instance)) {
				Issue(report, node.Path, "store refused document parent");
				rollback();
				return false;
			}
		}
		for (const PlannedProperty &property : properties) {
			if (property.Source->Type != PropertyType::Reference) continue;
			ecs::Entity target = ecs::NULL_ENTITY;
			if (!property.Source->Reference.empty()) {
				if (property.Source->Name == "Theme")
					target = themes[themeIndices.at(property.Source->Reference)].Instance;
				else
					target = nodes[indices.at(property.Source->Reference)].Instance;
			}
			if (!store.RestoreReference(nodes[property.Node].Instance, property.Descriptor->Name, target)) {
				Issue(report, property.Path, "store refused document reference", property.Source->SourceLine);
				rollback();
				return false;
			}
		}

		std::vector<ecs::Entity> completedRoots;
		completedRoots.reserve(document.Roots.size());
		for (const PlannedNode &node : nodes) {
			if (node.Parent == NO_PARENT) completedRoots.push_back(node.Instance);
		}
		roots = std::move(completedRoots);
		if (themesOut != nullptr) {
			themesOut->clear();
			themesOut->reserve(themes.size());
			for (const PlannedTheme &theme : themes)
				themesOut->push_back(theme.Instance);
		}
		return true;
	}

	bool DecodeAndImportDocument(
		ecs::Store &store,
		core::ByteReader &reader,
		std::vector<ecs::Entity> &roots,
		DocumentReport &report,
		const DocumentLimits &limits,
		std::vector<ecs::Entity> *themes
	) {
		UiDocument document;
		if (!DecodeDocument(reader, document, report, limits)) return false;
		return ImportDocument(store, document, roots, report, limits, themes);
	}

	bool
	ExportDocument(const ecs::Store &store, ecs::Entity root, UiDocument &document, DocumentReport &report) {
		report.Issues.clear();
		report.Truncated = false;
		document = {};
		RegisterGuiClasses();
		if (!store.Alive(root) || !DocumentCreatable(store.ClassOf(root)) ||
			!DocumentRoot(store.ClassOf(root))) {
			Issue(report, "$", "root is not a live concrete GUI document instance");
			return false;
		}
		std::unordered_map<ecs::Entity, std::string> ids;
		std::unordered_map<ecs::Entity, std::string> themeIds;
		uint32_t nextId = 1;
		uint32_t nextThemeId = 1;
		uint32_t nodes = 0;
		bool bounded = true;
		std::function<void(ecs::Entity, uint32_t)> assign = [&](ecs::Entity instance, uint32_t depth) {
			if (!bounded) return;
			if (depth > DocumentLimits::HARD_MAXIMUM_DEPTH || ++nodes > DocumentLimits::HARD_MAXIMUM_NODES) {
				Issue(report, "$", "GUI subtree exceeds document export limits");
				bounded = false;
				return;
			}
			const ecs::ClassInfo &info = ecs::Classes::Describe(store.ClassOf(instance));
			if (!TextWithin(info.Name.Text(), DocumentLimits{}) ||
				!TextWithin(store.InstanceNameOf(instance).Text(), DocumentLimits{})) {
				Issue(report, "$", "GUI subtree contains an oversized class or instance name");
				bounded = false;
				return;
			}
			ids.emplace(instance, "node" + std::to_string(nextId++));
			uint32_t children = 0;
			store.EachChild(instance, [&](ecs::Entity child) {
				if (!bounded || !DocumentCreatable(store.ClassOf(child))) return;
				if (++children > DocumentLimits::HARD_MAXIMUM_CHILDREN) {
					Issue(report, "$", "GUI subtree exceeds document child limit");
					bounded = false;
					return;
				}
				assign(child, depth + 1);
			});
		};
		assign(root, 1);
		if (!bounded) return false;
		auto exportTheme = [&](ecs::Entity theme, std::string_view path) -> const std::string * {
			const auto found = themeIds.find(theme);
			if (found != themeIds.end()) return &found->second;
			const UITheme *tokens = store.Get<UITheme>(theme);
			if (!store.Alive(theme) || tokens == nullptr ||
				document.Themes.size() >= DocumentLimits::HARD_MAXIMUM_THEMES) {
				Issue(report, std::string(path), "theme reference is invalid or exceeds document limits");
				return nullptr;
			}
			const std::string_view name = store.InstanceNameOf(theme).Text();
			if (!TextWithin(name, DocumentLimits{})) {
				Issue(report, std::string(path), "theme name exceeds document string limit");
				return nullptr;
			}
			DocumentTheme exported;
			exported.Id = "theme" + std::to_string(nextThemeId++);
			exported.Name = std::string(name);
			exported.Tokens = tokens->Tokens;
			document.Themes.push_back(std::move(exported));
			const auto [inserted, _] = themeIds.emplace(theme, document.Themes.back().Id);
			return &inserted->second;
		};
		std::function<bool(ecs::Entity, DocumentNode &)> write = [&](ecs::Entity instance,
																	 DocumentNode &node) {
			const ecs::ClassInfo &info = ecs::Classes::Describe(store.ClassOf(instance));
			node.Id = ids.at(instance);
			node.Class = std::string(info.Name.Text());
			node.Name = std::string(store.InstanceNameOf(instance).Text());
			auto writeComponent = [&](std::string_view name, const void *value) -> bool {
				if (value == nullptr) return true;
				const ecs::TypeDescriptor &descriptor =
					ecs::Components::Describe(ecs::Components::Find(core::Name(name)));
				if (!descriptor.Serialisable || descriptor.Write == nullptr) return false;
				try {
					core::ByteWriter payload(0, DocumentLimits::HARD_MAXIMUM_COMPONENT_BYTES);
					descriptor.Write(payload, value, 1);
					if (node.Components.size() >= DocumentLimits::HARD_MAXIMUM_COMPONENTS) return false;
					DocumentComponent exported;
					exported.Name = std::string(name);
					const std::span<const std::byte> bytes = payload.Bytes();
					exported.Bytes.assign(bytes.begin(), bytes.end());
					node.Components.push_back(std::move(exported));
					return true;
				} catch (const std::length_error &) {
					return false;
				}
			};
			if (!writeComponent("gui.UIStyle", store.Get<UIStyle>(instance)) ||
				!writeComponent("gui.AnimationPlayback", store.Get<AnimationPlayback>(instance)) ||
				!writeComponent("gui.VirtualCollection", store.Get<VirtualCollection>(instance)) ||
				!writeComponent("gui.Binding", store.Get<Binding>(instance)) ||
				!writeComponent("gui.StyleClass", store.Get<StyleClass>(instance)) ||
				!writeComponent("gui.StyleDirect", store.Get<StyleDirect>(instance)) ||
				!writeComponent("gui.LabelPresentation", store.Get<LabelPresentation>(instance)) ||
				!writeComponent(
					"gui.LabelLocalizationArguments", store.Get<LabelLocalizationArguments>(instance)
				)) {
				Issue(report, node.Id, "GUI authored component could not be exported");
				return false;
			}
			for (const PropertyDescriptor &descriptor : store.PropertiesOf(instance)) {
				if (!descriptor.Writable || descriptor.Spelling == "Name" ||
					descriptor.Spelling == "Parent" || descriptor.Type == PropertyType::Opaque ||
					!ExportStyledProperty(store, instance, descriptor))
					continue;
				if (node.Properties.size() >= DocumentLimits::HARD_MAXIMUM_PROPERTIES ||
					!TextWithin(descriptor.Spelling, DocumentLimits{})) {
					Issue(report, node.Id, "GUI subtree exceeds document property limits");
					return false;
				}
				DocumentProperty property;
				property.Name = std::string(descriptor.Spelling);
				if (descriptor.Type == PropertyType::Reference) {
					ecs::Entity target;
					if (!store.GetProperty(instance, descriptor, &target, sizeof(target))) return false;
					property.Type = PropertyType::Reference;
					property.Value.Type = PropertyType::Reference;
					if (target != ecs::NULL_ENTITY) {
						if (property.Name == "Theme") {
							const std::string *theme = exportTheme(target, node.Id + "." + property.Name);
							if (theme == nullptr) return false;
							property.Reference = *theme;
						} else {
							const auto found = ids.find(target);
							if (found == ids.end()) {
								Issue(
									report,
									node.Id + "." + property.Name,
									"reference points outside exported subtree"
								);
								return false;
							}
							property.Reference = found->second;
						}
					}
				} else if (!CopyProperty(store, instance, descriptor, property)) {
					Issue(report, node.Id + "." + property.Name, "property could not be read");
					return false;
				}
				if (!TextWithin(property.Value.String, DocumentLimits{}) ||
					!TextWithin(property.Reference, DocumentLimits{})) {
					Issue(
						report, node.Id + "." + property.Name, "GUI property exceeds document string limit"
					);
					return false;
				}
				node.Properties.push_back(std::move(property));
			}
			bool complete = true;
			uint32_t children = 0;
			store.EachChild(instance, [&](ecs::Entity child) {
				if (!complete || !DocumentCreatable(store.ClassOf(child))) return;
				if (++children > DocumentLimits::HARD_MAXIMUM_CHILDREN) {
					Issue(report, node.Id, "GUI subtree exceeds document child limit");
					complete = false;
					return;
				}
				node.Children.emplace_back();
				complete = write(child, node.Children.back()) && complete;
			});
			return complete;
		};
		document.Roots.emplace_back();
		if (!write(root, document.Roots.front())) {
			document = {};
			return false;
		}
		if (!ValidateDocument(document, report)) {
			document = {};
			return false;
		}
		return true;
	}
}
