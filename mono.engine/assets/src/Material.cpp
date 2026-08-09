#include <engine/assets/Material.hpp>

namespace engine::assets {

	bool Material::Write(core::ByteWriter &writer, const MaterialData &data) {
		if (!data.IsValid() || data.ColourMap.size() > MAXIMUM_NAME || data.NormalMap.size() > MAXIMUM_NAME ||
			data.RoughnessMap.size() > MAXIMUM_NAME || data.OcclusionMap.size() > MAXIMUM_NAME ||
			data.HeightMap.size() > MAXIMUM_NAME) {
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteString(data.ColourMap);

		// **Always written, even when every one is empty.** A writer that
		// omitted absent maps would make the record's length depend on its
		// contents, so a reader could not tell four empty names from a file that
		// stopped early — which is the distinction `reader.Failed()` below
		// exists to preserve.
		writer.WriteString(data.NormalMap);
		writer.WriteString(data.RoughnessMap);
		writer.WriteString(data.OcclusionMap);
		writer.WriteString(data.HeightMap);
		writer.WriteString(data.EmissiveMap);
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
		// **Version 1 is a colour map and nothing else**, which is this material
		// with four empty names — so it is read by not reading them rather than
		// by a second parser. See `VERSION`.
		std::string_view normal;
		std::string_view roughness;
		std::string_view occlusion;
		std::string_view height;
		std::string_view emissive;
		if (version >= 2) {
			normal = reader.ReadString();
			roughness = reader.ReadString();
			occlusion = reader.ReadString();
			height = reader.ReadString();

			if (reader.Failed() || normal.size() > MAXIMUM_NAME || roughness.size() > MAXIMUM_NAME ||
				occlusion.size() > MAXIMUM_NAME || height.size() > MAXIMUM_NAME) {
				return false;
			}
		}

		// **A third version for one more string, read the way the second was.**
		// Each version adds fields at the end and never reorders them, so
		// reading an older file is reading fewer strings rather than parsing a
		// different layout.
		if (version >= 3) {
			emissive = reader.ReadString();
			if (reader.Failed() || emissive.size() > MAXIMUM_NAME) {
				return false;
			}
		}

		out.ColourMap.assign(colour);
		out.NormalMap.assign(normal);
		out.RoughnessMap.assign(roughness);
		out.OcclusionMap.assign(occlusion);
		out.HeightMap.assign(height);
		out.EmissiveMap.assign(emissive);
		return true;
	}
}
