#include <engine/assets/Material.hpp>

namespace engine::assets {

	bool Material::Write(core::ByteWriter &writer, const MaterialData &data) {
		if (!data.IsValid() || data.ColourMap.size() > MAXIMUM_NAME) {
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteString(data.ColourMap);
		return true;
	}

	bool Material::Read(core::ByteReader &reader, MaterialData &out) {
		if (reader.ReadUInt32() != MAGIC) {
			return false;
		}
		const uint16_t version = reader.ReadUInt16();
		if (version == 0 || version > VERSION) {
			return false;
		}

		const std::string_view colour = reader.ReadString();

		// **`Failed()` and not the emptiness of the view**, because an empty
		// colour map is a material somebody authored and has not textured yet —
		// `MaterialData::ColourMap` says so — and a truncated file reads back as
		// exactly the same empty view. Without this the two are one answer, and
		// the wrong half of it draws the default and reports nothing.
		if (reader.Failed() || colour.size() > MAXIMUM_NAME) {
			return false;
		}

		// **Assigned last, so a refusal leaves `out` alone.** A caller reusing
		// one across a run of files would otherwise act on a mixture of the last
		// good material and a bad one — `Read`'s contract, and `Texture::Read`
		// keeps the same promise the same way.
		out.ColourMap.assign(colour);
		return true;
	}
}
