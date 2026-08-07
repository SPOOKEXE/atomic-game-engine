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
