#include <engine/ecs/EnumTable.hpp>
#include <engine/game/Values.hpp>

#include <array>
#include <charconv>
#include <cstring>
#include <system_error>

namespace engine::game {

	namespace {
		using ecs::PropertyType;

		// Shortest round-trip form, and the reason it is not `std::to_string`.
		//
		// `to_string` is `%f` — six decimal places, so 1e-8 writes as "0.000000"
		// and 1e20 writes as twenty-one digits of noise. `to_chars` without a
		// precision produces the shortest text that reads back as the same
		// float, which is the property a save file needs: loading and re-saving
		// a scene has to leave it byte-identical, or every round trip through
		// the editor moves everything slightly.
		std::string Number(float value) {
			std::array<char, 32> buffer{};
			const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			if (result.ec != std::errc{}) {
				return "0";
			}
			return std::string(buffer.data(), result.ptr);
		}

		std::string Number(double value) {
			std::array<char, 40> buffer{};
			const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			if (result.ec != std::errc{}) {
				return "0";
			}
			return std::string(buffer.data(), result.ptr);
		}

		std::string_view Trim(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
									 text.front() == '\r')) {
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
									 text.back() == '\r')) {
				text.remove_suffix(1);
			}
			return text;
		}

		// Splits on commas and reads exactly `count` numbers.
		//
		// Exactly, not at least. A `Vector3` given four numbers is a mistake
		// somebody made, and quietly using the first three would put the part
		// somewhere plausible and leave the mistake in the file.
		bool Numbers(std::string_view text, size_t count, float *out) {
			size_t found = 0;
			size_t at = 0;

			while (at <= text.size()) {
				const size_t comma = text.find(',', at);
				const std::string_view piece = Trim(
					text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at)
				);

				if (found >= count || piece.empty()) {
					return false;
				}

				const auto result = std::from_chars(piece.data(), piece.data() + piece.size(), out[found]);
				if (result.ec != std::errc{} || result.ptr != piece.data() + piece.size()) {
					return false;
				}

				found++;
				if (comma == std::string_view::npos) {
					break;
				}
				at = comma + 1;
			}

			return found == count;
		}

		template <class T> bool Integer(std::string_view text, T &out) {
			const std::string_view piece = Trim(text);
			if (piece.empty()) {
				return false;
			}
			const auto result = std::from_chars(piece.data(), piece.data() + piece.size(), out);
			return result.ec == std::errc{} && result.ptr == piece.data() + piece.size();
		}

		bool Real(std::string_view text, float &out) {
			return Numbers(text, 1, &out);
		}

		bool Real(std::string_view text, double &out) {
			const std::string_view piece = Trim(text);
			if (piece.empty()) {
				return false;
			}
			const auto result = std::from_chars(piece.data(), piece.data() + piece.size(), out);
			return result.ec == std::errc{} && result.ptr == piece.data() + piece.size();
		}
	}

	std::string FormatNumber(double value) {
		return Number(value);
	}

	bool ReadProperty(
		const ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &descriptor,
		PropertyValue &out
	) {
		out = PropertyValue{};
		out.Type = descriptor.Type;

		if (descriptor.Get == nullptr) {
			return false;
		}

		// **Through `Store::GetProperty` rather than `descriptor.Get`**, so the
		// size check runs. A descriptor whose `Size` disagrees with the field
		// this switch hands it would otherwise write past the end of a local.
		switch (descriptor.Type) {
		case PropertyType::Bool:
			return store.GetProperty(instance, descriptor, &out.Bool, sizeof(out.Bool));
		case PropertyType::Int32:
			return store.GetProperty(instance, descriptor, &out.Int32, sizeof(out.Int32));
		case PropertyType::Int64:
			return store.GetProperty(instance, descriptor, &out.Int64, sizeof(out.Int64));
		case PropertyType::Float:
			return store.GetProperty(instance, descriptor, &out.Float, sizeof(out.Float));
		case PropertyType::Double:
			return store.GetProperty(instance, descriptor, &out.Double, sizeof(out.Double));
		case PropertyType::Name:
		case PropertyType::Enum:
			return store.GetProperty(instance, descriptor, &out.Name, sizeof(out.Name));
		case PropertyType::Reference:
			return store.GetProperty(instance, descriptor, &out.Reference, sizeof(out.Reference));
		case PropertyType::Vector3:
			return store.GetProperty(instance, descriptor, &out.Vector3, sizeof(out.Vector3));
		case PropertyType::CFrame:
			return store.GetProperty(instance, descriptor, &out.CFrame, sizeof(out.CFrame));
		case PropertyType::Color3:
			return store.GetProperty(instance, descriptor, &out.Color3, sizeof(out.Color3));
		case PropertyType::Vector2:
			return store.GetProperty(instance, descriptor, &out.Vector2, sizeof(out.Vector2));
		case PropertyType::UDim:
			return store.GetProperty(instance, descriptor, &out.UDim, sizeof(out.UDim));
		case PropertyType::UDim2:
			return store.GetProperty(instance, descriptor, &out.UDim2, sizeof(out.UDim2));
		case PropertyType::Rect:
			return store.GetProperty(instance, descriptor, &out.Rect, sizeof(out.Rect));
		case PropertyType::Opaque:
			// Readable as bytes and not as a value. Nothing here can show
			// it and nothing here should pretend to.
			return false;
		}
		return false;
	}

	bool WriteProperty(
		ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &descriptor,
		const PropertyValue &value
	) {
		if (value.Type != descriptor.Type || !descriptor.Writable) {
			return false;
		}

		switch (descriptor.Type) {
		case PropertyType::Bool:
			return store.SetProperty(instance, descriptor, &value.Bool, sizeof(value.Bool));
		case PropertyType::Int32:
			return store.SetProperty(instance, descriptor, &value.Int32, sizeof(value.Int32));
		case PropertyType::Int64:
			return store.SetProperty(instance, descriptor, &value.Int64, sizeof(value.Int64));
		case PropertyType::Float:
			return store.SetProperty(instance, descriptor, &value.Float, sizeof(value.Float));
		case PropertyType::Double:
			return store.SetProperty(instance, descriptor, &value.Double, sizeof(value.Double));
		case PropertyType::Name:
			return store.SetProperty(instance, descriptor, &value.Name, sizeof(value.Name));
		case PropertyType::Enum:
			// **Checked here as well as at the binding.** A game file is
			// text somebody could have edited, so `Material` reading
			// "Plsatic" has to be refused where it was read rather than
			// landing in the component and surfacing as a part drawn with
			// the default for reasons nobody can see — which is exactly what
			// `PropertyType::Enum` was added to prevent.
			if (descriptor.EnumName.IsValid() && !ecs::EnumTable::Has(descriptor.EnumName, value.Name)) {
				return false;
			}
			return store.SetProperty(instance, descriptor, &value.Name, sizeof(value.Name));
		case PropertyType::Reference:
			return store.SetProperty(instance, descriptor, &value.Reference, sizeof(value.Reference));
		case PropertyType::Vector3:
			return store.SetProperty(instance, descriptor, &value.Vector3, sizeof(value.Vector3));
		case PropertyType::CFrame:
			return store.SetProperty(instance, descriptor, &value.CFrame, sizeof(value.CFrame));
		case PropertyType::Color3:
			return store.SetProperty(instance, descriptor, &value.Color3, sizeof(value.Color3));
		case PropertyType::Vector2:
			return store.SetProperty(instance, descriptor, &value.Vector2, sizeof(value.Vector2));
		case PropertyType::UDim:
			return store.SetProperty(instance, descriptor, &value.UDim, sizeof(value.UDim));
		case PropertyType::UDim2:
			return store.SetProperty(instance, descriptor, &value.UDim2, sizeof(value.UDim2));
		case PropertyType::Rect:
			return store.SetProperty(instance, descriptor, &value.Rect, sizeof(value.Rect));
		case PropertyType::Opaque:
			return false;
		}
		return false;
	}

	std::string FormatValue(const PropertyValue &value) {
		switch (value.Type) {
		case PropertyType::Bool:
			return value.Bool ? "true" : "false";
		case PropertyType::Int32:
			return std::to_string(value.Int32);
		case PropertyType::Int64:
			return std::to_string(value.Int64);
		case PropertyType::Float:
			return Number(value.Float);
		case PropertyType::Double:
			return Number(value.Double);
		case PropertyType::Name:
		case PropertyType::Enum:
			return value.Name.IsValid() ? std::string(value.Name.Text()) : std::string{};
		case PropertyType::Vector3:
			return Number(value.Vector3.X) + ", " + Number(value.Vector3.Y) + ", " + Number(value.Vector3.Z);
		case PropertyType::Color3:
			return Number(value.Color3.R) + ", " + Number(value.Color3.G) + ", " + Number(value.Color3.B);
		case PropertyType::CFrame:
			// Position then quaternion — seven numbers, and the quaternion
			// rather than Roblox's nine-float rotation matrix because that
			// is what `core::CFrame` stores. Writing a matrix would mean
			// converting on the way out and orthonormalising on the way
			// back in, which is a round trip that does not return what it
			// was given.
			return Number(value.CFrame.Position.X) + ", " + Number(value.CFrame.Position.Y) + ", " +
				   Number(value.CFrame.Position.Z) + ", " + Number(value.CFrame.QuaternionX) + ", " +
				   Number(value.CFrame.QuaternionY) + ", " + Number(value.CFrame.QuaternionZ) + ", " +
				   Number(value.CFrame.QuaternionW);
		case PropertyType::Vector2:
			return Number(value.Vector2.X) + ", " + Number(value.Vector2.Y);
		case PropertyType::UDim:
			// Scale then offset, which is `UDim.new`'s argument order. The
			// comma-separated list is the same shape every other value here
			// uses, so one parser reads all of them.
			return Number(value.UDim.Scale) + ", " + Number(value.UDim.Offset);
		case PropertyType::UDim2:
			// Four numbers in `UDim2.new`'s order — xScale, xOffset, yScale,
			// yOffset — and **not** two `UDim` texts joined, which would be a
			// second nesting the parser would have to learn.
			return Number(value.UDim2.X.Scale) + ", " + Number(value.UDim2.X.Offset) + ", " +
				   Number(value.UDim2.Y.Scale) + ", " + Number(value.UDim2.Y.Offset);
		case PropertyType::Rect:
			// `Rect.new`'s order: minX, minY, maxX, maxY. `core::Rect` stores
			// two corners and not a corner and an extent, so this is the fields
			// in order rather than a conversion.
			return Number(value.Rect.Min.X) + ", " + Number(value.Rect.Min.Y) + ", " +
				   Number(value.Rect.Max.X) + ", " + Number(value.Rect.Max.Y);
		case PropertyType::Reference:
		case PropertyType::Opaque:
			return {};
		}
		return {};
	}

	bool ParseValue(ecs::PropertyType type, std::string_view text, PropertyValue &out, std::string &reason) {
		out = PropertyValue{};
		out.Type = type;
		reason.clear();

		switch (type) {
		case PropertyType::Bool: {
			const std::string_view piece = Trim(text);
			if (piece == "true") {
				out.Bool = true;
				return true;
			}
			if (piece == "false") {
				out.Bool = false;
				return true;
			}
			reason = "expected true or false";
			return false;
		}

		case PropertyType::Int32:
			if (!Integer(text, out.Int32)) {
				reason = "expected a whole number";
				return false;
			}
			return true;

		case PropertyType::Int64:
			if (!Integer(text, out.Int64)) {
				reason = "expected a whole number";
				return false;
			}
			return true;

		case PropertyType::Float:
			if (!Real(text, out.Float)) {
				reason = "expected a number";
				return false;
			}
			return true;

		case PropertyType::Double:
			if (!Real(text, out.Double)) {
				reason = "expected a number";
				return false;
			}
			return true;

		case PropertyType::Name:
		case PropertyType::Enum: {
			// **Not trimmed, and not rejected when empty.** An instance
			// name may legitimately have a leading space and an empty name
			// is a legal state — `ecs::InstanceName` says so. Trimming here
			// would be this file quietly deciding what a name is.
			out.Name = text.empty() ? core::Name{} : core::Name(text);
			return true;
		}

		case PropertyType::Vector3: {
			float parts[3]{};
			if (!Numbers(text, 3, parts)) {
				reason = "expected x, y, z";
				return false;
			}
			out.Vector3 = core::Vector3{parts[0], parts[1], parts[2]};
			return true;
		}

		case PropertyType::Color3: {
			float parts[3]{};
			if (!Numbers(text, 3, parts)) {
				reason = "expected r, g, b";
				return false;
			}
			out.Color3 = core::Color3{parts[0], parts[1], parts[2]};
			return true;
		}

		case PropertyType::CFrame: {
			float parts[7]{};
			if (!Numbers(text, 7, parts)) {
				reason = "expected x, y, z, qx, qy, qz, qw";
				return false;
			}
			out.CFrame.Position = core::Vector3{parts[0], parts[1], parts[2]};
			out.CFrame.QuaternionX = parts[3];
			out.CFrame.QuaternionY = parts[4];
			out.CFrame.QuaternionZ = parts[5];
			out.CFrame.QuaternionW = parts[6];
			return true;
		}

		case PropertyType::Vector2: {
			float parts[2]{};
			if (!Numbers(text, 2, parts)) {
				reason = "expected x, y";
				return false;
			}
			out.Vector2 = core::Vector2{parts[0], parts[1]};
			return true;
		}

		case PropertyType::UDim: {
			float parts[2]{};
			if (!Numbers(text, 2, parts)) {
				reason = "expected scale, offset";
				return false;
			}
			out.UDim = core::UDim{parts[0], parts[1]};
			return true;
		}

		case PropertyType::UDim2: {
			float parts[4]{};
			if (!Numbers(text, 4, parts)) {
				reason = "expected xScale, xOffset, yScale, yOffset";
				return false;
			}
			out.UDim2 = core::UDim2{parts[0], parts[1], parts[2], parts[3]};
			return true;
		}

		case PropertyType::Rect: {
			float parts[4]{};
			if (!Numbers(text, 4, parts)) {
				reason = "expected minX, minY, maxX, maxY";
				return false;
			}
			out.Rect = core::Rect{parts[0], parts[1], parts[2], parts[3]};
			return true;
		}

		case PropertyType::Reference:
			reason = "a reference has no text form";
			return false;

		case PropertyType::Opaque:
			reason = "this property has no readable value";
			return false;
		}

		reason = "unknown property type";
		return false;
	}

	bool ValuesEqual(const PropertyValue &left, const PropertyValue &right) {
		if (left.Type != right.Type) {
			return false;
		}

		switch (left.Type) {
		case PropertyType::Bool:
			return left.Bool == right.Bool;
		case PropertyType::Int32:
			return left.Int32 == right.Int32;
		case PropertyType::Int64:
			return left.Int64 == right.Int64;
		case PropertyType::Float:
			return left.Float == right.Float;
		case PropertyType::Double:
			return left.Double == right.Double;
		case PropertyType::Name:
		case PropertyType::Enum:
			return left.Name == right.Name;
		case PropertyType::Reference:
			return left.Reference == right.Reference;
		case PropertyType::Vector3:
			return left.Vector3.X == right.Vector3.X && left.Vector3.Y == right.Vector3.Y &&
				   left.Vector3.Z == right.Vector3.Z;
		case PropertyType::Color3:
			return left.Color3.R == right.Color3.R && left.Color3.G == right.Color3.G &&
				   left.Color3.B == right.Color3.B;
		case PropertyType::CFrame:
			return std::memcmp(&left.CFrame, &right.CFrame, sizeof(core::CFrame)) == 0;
		// **Their own `operator==`, not a `memcmp`.** All four are packed floats
		// today and a byte compare would agree — until one of them gains a
		// padding byte, at which point a byte compare reports two equal values
		// as different and the document writes a property that did not change.
		// The `CFrame` case above is the exception and is a `memcmp` because
		// `core::CFrame` declares no equality.
		case PropertyType::Vector2:
			return left.Vector2 == right.Vector2;
		case PropertyType::UDim:
			return left.UDim == right.UDim;
		case PropertyType::UDim2:
			return left.UDim2 == right.UDim2;
		case PropertyType::Rect:
			return left.Rect == right.Rect;
		case PropertyType::Opaque:
			// Two values nobody can read are never equal, so an `Opaque`
			// property is always written. There are none today; when there
			// is one, "always written" is the conservative answer.
			return false;
		}
		return false;
	}

	std::string_view TypeTag(ecs::PropertyType type) {
		switch (type) {
		case PropertyType::Bool:
			return "bool";
		case PropertyType::Int32:
			return "int";
		case PropertyType::Int64:
			return "int64";
		case PropertyType::Float:
			return "float";
		case PropertyType::Double:
			return "double";
		case PropertyType::Name:
			return "string";
		case PropertyType::Enum:
			return "enum";
		case PropertyType::Reference:
			return "ref";
		case PropertyType::Vector3:
			return "Vector3";
		case PropertyType::CFrame:
			return "CFrame";
		case PropertyType::Color3:
			return "Color3";
		case PropertyType::Vector2:
			return "Vector2";
		case PropertyType::UDim:
			return "UDim";
		case PropertyType::UDim2:
			return "UDim2";
		case PropertyType::Rect:
			return "Rect";
		case PropertyType::Opaque:
			return "opaque";
		}
		return "opaque";
	}

	bool TypeFromTag(std::string_view tag, ecs::PropertyType &out) {
		static constexpr std::array<std::pair<std::string_view, PropertyType>, 16> TAGS{{
			{"bool", PropertyType::Bool},
			{"int", PropertyType::Int32},
			{"int64", PropertyType::Int64},
			{"float", PropertyType::Float},
			{"double", PropertyType::Double},
			{"string", PropertyType::Name},
			{"enum", PropertyType::Enum},
			{"ref", PropertyType::Reference},
			{"Vector3", PropertyType::Vector3},
			{"CFrame", PropertyType::CFrame},
			{"Color3", PropertyType::Color3},
			{"Vector2", PropertyType::Vector2},
			{"UDim", PropertyType::UDim},
			{"UDim2", PropertyType::UDim2},
			{"Rect", PropertyType::Rect},
			{"opaque", PropertyType::Opaque},
		}};

		for (const auto &entry : TAGS) {
			if (entry.first == tag) {
				out = entry.second;
				return true;
			}
		}
		return false;
	}
}
