#include <engine/assets/Material.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.assets.material")

using engine::assets::Material;
using engine::assets::MaterialData;
using engine::core::ByteReader;
using engine::core::ByteWriter;

namespace {
	MaterialData RoundTrip(const MaterialData &input, bool &wrote, bool &read) {
		ByteWriter writer;
		wrote = Material::Write(writer, input);

		MaterialData out;
		ByteReader reader(writer.Bytes());
		read = Material::Read(reader, out);
		return out;
	}
}

TEST_CASE("a material round-trips its colour map", "[assets]") {
	MaterialData input;
	input.ColourMap = "materials/ambientcg/Bricks075A_Color.atex";

	bool wrote = false;
	bool read = false;
	const MaterialData out = RoundTrip(input, wrote, read);

	CHECK(wrote);
	CHECK(read);
	CHECK(out.ColourMap == input.ColourMap);
}

TEST_CASE("a material with no colour map is a material", "[assets]") {
	// The null case is representable on purpose: a material somebody authored
	// and has not textured yet is a different fact from no material at all, and
	// refusing this would collapse the two. See `MaterialData::ColourMap`.
	bool wrote = false;
	bool read = false;
	const MaterialData out = RoundTrip(MaterialData{}, wrote, read);

	CHECK(wrote);
	CHECK(read);
	CHECK(out.ColourMap.empty());
}

TEST_CASE("anything that is not a material is refused", "[assets]") {
	SECTION("wrong magic") {
		ByteWriter writer;
		writer.WriteUInt32(0x31585441); // Texture's, which is the near miss that matters
		writer.WriteUInt16(1);
		writer.WriteString("x.atex");

		MaterialData out;
		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Material::Read(reader, out));
	}

	SECTION("a version this build does not know") {
		ByteWriter writer;
		writer.WriteUInt32(Material::MAGIC);
		writer.WriteUInt16(Material::VERSION + 1);
		writer.WriteString("x.atex");

		MaterialData out;
		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Material::Read(reader, out));
	}

	SECTION("version zero") {
		ByteWriter writer;
		writer.WriteUInt32(Material::MAGIC);
		writer.WriteUInt16(0);
		writer.WriteString("x.atex");

		MaterialData out;
		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Material::Read(reader, out));
	}

	SECTION("truncated before the name") {
		// **The case `Failed()` exists for.** A truncated file reads back the
		// same empty view an untextured material does, so a reader checking only
		// emptiness would accept this and draw the default with nothing said.
		ByteWriter writer;
		writer.WriteUInt32(Material::MAGIC);
		writer.WriteUInt16(Material::VERSION);

		MaterialData out;
		out.ColourMap = "kept.atex";
		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Material::Read(reader, out));

		// Left alone on refusal, so a caller reusing one across a run of files
		// cannot act on a mixture of the last good material and a bad one.
		CHECK(out.ColourMap == "kept.atex");
	}
}

TEST_CASE("a name past the ceiling is refused rather than written", "[assets]") {
	// The bound is on both ends: a writer that emitted one would produce a file
	// its own reader refuses, which is a corrupt buffer produced by the writer.
	MaterialData input;
	input.ColourMap = std::string(Material::MAXIMUM_NAME + 1, 'a');

	ByteWriter writer;
	CHECK_FALSE(Material::Write(writer, input));
	CHECK(writer.Bytes().empty());
}

TEST_CASE("a material round-trips the version 2 PBR maps", "[assets]") {
	// The four `ROADMAP.md` v0.10 published and nothing could name until the
	// G-buffer existed to sample them.
	MaterialData written;
	written.ColourMap = "materials/bricks_colour.atex";
	written.NormalMap = "materials/bricks_normal.atex";
	written.RoughnessMap = "materials/bricks_roughness.atex";
	written.OcclusionMap = "materials/bricks_occlusion.atex";
	written.HeightMap = "materials/bricks_height.atex";

	ByteWriter writer;
	REQUIRE(Material::Write(writer, written));

	ByteReader reader(writer.Bytes());
	MaterialData read;
	REQUIRE(Material::Read(reader, read));

	CHECK(read.ColourMap == written.ColourMap);
	CHECK(read.NormalMap == written.NormalMap);
	CHECK(read.RoughnessMap == written.RoughnessMap);
	CHECK(read.OcclusionMap == written.OcclusionMap);
	CHECK(read.HeightMap == written.HeightMap);
}

TEST_CASE("a version 1 material is one with four empty maps", "[assets]") {
	// **The older format is not a special case to translate.** A version 1 file
	// is a colour map and nothing else, which is exactly this material with four
	// absent names - so it is read by not reading them. See `Material::VERSION`.
	//
	// Hand-built rather than produced by an older writer, because there is no
	// older writer to run: the point is that bytes already on disk still load.
	ByteWriter writer;
	writer.WriteUInt32(Material::MAGIC);
	writer.WriteUInt16(1);
	writer.WriteString("materials/old_colour.atex");

	ByteReader reader(writer.Bytes());
	MaterialData read;

	// Seeded with rubbish, so "left empty" is a thing this can observe rather
	// than the default it started at.
	read.NormalMap = "not overwritten";

	REQUIRE(Material::Read(reader, read));
	CHECK(read.ColourMap == "materials/old_colour.atex");
	CHECK(read.NormalMap.empty());
	CHECK(read.RoughnessMap.empty());
	CHECK(read.OcclusionMap.empty());
	CHECK(read.HeightMap.empty());
}

TEST_CASE("a material truncated after its colour map is refused", "[assets]") {
	// **What the always-written maps buy.** A version 2 file that stops after
	// the colour map is a truncation, and it has to fail as one rather than read
	// back as a material whose other four names happen to be empty.
	ByteWriter writer;
	writer.WriteUInt32(Material::MAGIC);
	writer.WriteUInt16(Material::VERSION);
	writer.WriteString("materials/bricks_colour.atex");

	ByteReader reader(writer.Bytes());
	MaterialData read;
	CHECK_FALSE(Material::Read(reader, read));
}

TEST_CASE("a material round-trips its emissive map", "[assets]") {
	// The sixth name, and the one with no CC0 source behind it - see
	// `MaterialData::EmissiveMap` for why it is authored rather than fetched.
	MaterialData written;
	written.ColourMap = "signs/neon_colour.atex";
	written.EmissiveMap = "signs/neon_emissive.atex";

	ByteWriter writer;
	REQUIRE(Material::Write(writer, written));

	ByteReader reader(writer.Bytes());
	MaterialData read;
	REQUIRE(Material::Read(reader, read));

	CHECK(read.ColourMap == written.ColourMap);
	CHECK(read.EmissiveMap == written.EmissiveMap);
}

TEST_CASE("a material round-trips its metalness map", "[assets]") {
	MaterialData written;
	written.ColourMap = "metal/colour.atex";
	written.MetalnessMap = "metal/metalness.atex";

	ByteWriter writer;
	REQUIRE(Material::Write(writer, written));

	ByteReader reader(writer.Bytes());
	MaterialData read;
	REQUIRE(Material::Read(reader, read));
	CHECK(read.MetalnessMap == written.MetalnessMap);
}

TEST_CASE("a version 3 material is one with no metalness map", "[assets]") {
	ByteWriter writer;
	writer.WriteUInt32(Material::MAGIC);
	writer.WriteUInt16(3);
	writer.WriteString("materials/colour.atex");
	writer.WriteString("materials/normal.atex");
	writer.WriteString("materials/roughness.atex");
	writer.WriteString("materials/occlusion.atex");
	writer.WriteString("materials/height.atex");
	writer.WriteString("materials/emissive.atex");

	ByteReader reader(writer.Bytes());
	MaterialData read;
	read.MetalnessMap = "not overwritten";
	REQUIRE(Material::Read(reader, read));
	CHECK(read.EmissiveMap == "materials/emissive.atex");
	CHECK(read.MetalnessMap.empty());
}

TEST_CASE("every material map obeys the name ceiling", "[assets]") {
	MaterialData input;
	input.EmissiveMap = std::string(Material::MAXIMUM_NAME + 1, 'e');
	ByteWriter writer;
	CHECK_FALSE(Material::Write(writer, input));
	CHECK(writer.Bytes().empty());

	input.EmissiveMap.clear();
	input.MetalnessMap = std::string(Material::MAXIMUM_NAME + 1, 'm');
	CHECK_FALSE(Material::Write(writer, input));
	CHECK(writer.Bytes().empty());
}

TEST_CASE("a version 2 material is one with no emissive map", "[assets]") {
	// **Each version adds fields and never reorders them**, so reading an older
	// file is reading fewer strings rather than a different layout. Version 2 is
	// five names; this checks the sixth is absent rather than misread from
	// whatever followed.
	ByteWriter writer;
	writer.WriteUInt32(Material::MAGIC);
	writer.WriteUInt16(2);
	writer.WriteString("materials/bricks_colour.atex");
	writer.WriteString("materials/bricks_normal.atex");
	writer.WriteString("materials/bricks_roughness.atex");
	writer.WriteString("materials/bricks_occlusion.atex");
	writer.WriteString("materials/bricks_height.atex");

	ByteReader reader(writer.Bytes());
	MaterialData read;
	read.EmissiveMap = "not overwritten";

	REQUIRE(Material::Read(reader, read));
	CHECK(read.NormalMap == "materials/bricks_normal.atex");
	CHECK(read.HeightMap == "materials/bricks_height.atex");
	CHECK(read.EmissiveMap.empty());
}
