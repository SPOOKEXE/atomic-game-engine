#include <engine/assets/Material.hpp>

#include <cmath>

namespace engine::assets {

	bool Material::Write(core::ByteWriter &writer, const MaterialData &data) {
		if (!data.IsValid() || data.ColourMap.size() > MAXIMUM_NAME || data.NormalMap.size() > MAXIMUM_NAME ||
			data.RoughnessMap.size() > MAXIMUM_NAME || data.OcclusionMap.size() > MAXIMUM_NAME ||
			data.HeightMap.size() > MAXIMUM_NAME || data.EmissiveMap.size() > MAXIMUM_NAME ||
			data.MetalnessMap.size() > MAXIMUM_NAME || data.PackedPbrMap.size() > MAXIMUM_NAME ||
			(data.RoughnessChannel > 3 && data.RoughnessChannel != 255) ||
			(data.OcclusionChannel > 3 && data.OcclusionChannel != 255) ||
			(data.HeightChannel > 3 && data.HeightChannel != 255) ||
			(data.MetalnessChannel > 3 && data.MetalnessChannel != 255) ||
			!std::isfinite(data.SpecularFactor) || data.SpecularFactor < 0.0f || data.SpecularFactor > 1.0f ||
			!std::isfinite(data.TransmissionFactor) || data.TransmissionFactor < 0.0f ||
			data.TransmissionFactor > 1.0f || !std::isfinite(data.IndexOfRefraction) ||
			data.IndexOfRefraction < 1.0f || data.IndexOfRefraction > 3.0f ||
			!std::isfinite(data.Thickness) || data.Thickness < 0.0f || data.Thickness > 1000.0f) {
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteString(data.ColourMap);

		// **Always written, even when every one is empty.** A writer that
		// omitted absent maps would make the record's length depend on its
		// contents, so a reader could not tell four empty names from a file that
		// stopped early - which is the distinction `reader.Failed()` below
		// exists to preserve.
		writer.WriteString(data.NormalMap);
		writer.WriteString(data.RoughnessMap);
		writer.WriteString(data.OcclusionMap);
		writer.WriteString(data.HeightMap);
		writer.WriteString(data.EmissiveMap);
		writer.WriteString(data.MetalnessMap);
		writer.WriteString(data.PackedPbrMap);
		writer.WriteUInt8(data.RoughnessChannel);
		writer.WriteUInt8(data.OcclusionChannel);
		writer.WriteUInt8(data.HeightChannel);
		writer.WriteUInt8(data.MetalnessChannel);
		writer.WriteFloat(data.SpecularFactor);
		writer.WriteFloat(data.TransmissionFactor);
		writer.WriteFloat(data.IndexOfRefraction);
		writer.WriteFloat(data.Thickness);
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
		// colour map is a material somebody authored and has not textured yet -
		// `MaterialData::ColourMap` says so - and a truncated file reads back as
		// exactly the same empty view. Without this the two are one answer, and
		// the wrong half of it draws the default and reports nothing.
		if (reader.Failed() || colour.size() > MAXIMUM_NAME) {
			return false;
		}

		// **Assigned last, so a refusal leaves `out` alone.** A caller reusing
		// one across a run of files would otherwise act on a mixture of the last
		// good material and a bad one - `Read`'s contract, and `Texture::Read`
		// keeps the same promise the same way.
		// **Version 1 is a colour map and nothing else**, which is this material
		// with four empty names - so it is read by not reading them rather than
		// by a second parser. See `VERSION`.
		std::string_view normal;
		std::string_view roughness;
		std::string_view occlusion;
		std::string_view height;
		std::string_view emissive;
		std::string_view metalness;
		std::string_view packedPbr;
		uint8_t roughnessChannel = 255, occlusionChannel = 255, heightChannel = 255, metalnessChannel = 255;
		float specularFactor = 1.0f, transmissionFactor = 0.0f;
		float indexOfRefraction = 1.5f, thickness = 0.0f;
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

		if (version >= 4) {
			metalness = reader.ReadString();
			if (reader.Failed() || metalness.size() > MAXIMUM_NAME) {
				return false;
			}
		}
		if (version >= 5) {
			packedPbr = reader.ReadString();
			roughnessChannel = reader.ReadUInt8();
			occlusionChannel = reader.ReadUInt8();
			heightChannel = reader.ReadUInt8();
			metalnessChannel = reader.ReadUInt8();
			if (version >= 6) {
				specularFactor = reader.ReadFloat();
				transmissionFactor = reader.ReadFloat();
			}
			if (version >= 7) {
				indexOfRefraction = reader.ReadFloat();
				thickness = reader.ReadFloat();
			}
			if (reader.Failed() || packedPbr.size() > MAXIMUM_NAME ||
				(roughnessChannel > 3 && roughnessChannel != 255) ||
				(occlusionChannel > 3 && occlusionChannel != 255) ||
				(heightChannel > 3 && heightChannel != 255) ||
				(metalnessChannel > 3 && metalnessChannel != 255) || !std::isfinite(specularFactor) ||
				specularFactor < 0.0f || specularFactor > 1.0f || !std::isfinite(transmissionFactor) ||
				transmissionFactor < 0.0f || transmissionFactor > 1.0f || !std::isfinite(indexOfRefraction) ||
				indexOfRefraction < 1.0f || indexOfRefraction > 3.0f || !std::isfinite(thickness) ||
				thickness < 0.0f || thickness > 1000.0f) {
				return false;
			}
		}

		out.ColourMap.assign(colour);
		out.NormalMap.assign(normal);
		out.RoughnessMap.assign(roughness);
		out.OcclusionMap.assign(occlusion);
		out.HeightMap.assign(height);
		out.EmissiveMap.assign(emissive);
		out.MetalnessMap.assign(metalness);
		out.PackedPbrMap.assign(packedPbr);
		out.RoughnessChannel = roughnessChannel;
		out.OcclusionChannel = occlusionChannel;
		out.HeightChannel = heightChannel;
		out.MetalnessChannel = metalnessChannel;
		out.SpecularFactor = specularFactor;
		out.TransmissionFactor = transmissionFactor;
		out.IndexOfRefraction = indexOfRefraction;
		out.Thickness = thickness;
		return true;
	}
}
