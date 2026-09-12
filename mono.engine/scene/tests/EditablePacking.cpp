#include <engine/scene/EditablePacking.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.scene.editablepacking")

using Catch::Approx;
using engine::scene::EditablePackedByteCount;
using engine::scene::EditablePacking;
using engine::scene::EditablePackingFormat;
using engine::scene::PackEditableValues;
using engine::scene::UnpackEditableValues;

TEST_CASE(
	"editable packing has deterministic byte counts including packed tails", "[scene][editablepacking]"
) {
	CHECK(EditablePackedByteCount(EditablePackingFormat::Float16, 3) == 6);
	CHECK(EditablePackedByteCount(EditablePackingFormat::Float8E4M3FN, 3) == 3);
	CHECK(EditablePackedByteCount(EditablePackingFormat::Signed16, 3) == 6);
	CHECK(EditablePackedByteCount(EditablePackingFormat::Unsigned8, 3) == 3);
	CHECK(EditablePackedByteCount(EditablePackingFormat::Signed4, 3) == 2);
	CHECK(EditablePackedByteCount(EditablePackingFormat::Boolean, 9) == 2);
}

TEST_CASE("every editable packing format round trips within its declared error", "[scene][editablepacking]") {
	const std::array<float, 5> values{-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
	for (const auto format :
		 {EditablePackingFormat::Float32,
		  EditablePackingFormat::Float16,
		  EditablePackingFormat::Float8E4M3FN,
		  EditablePackingFormat::Signed16,
		  EditablePackingFormat::Unsigned16,
		  EditablePackingFormat::Signed8,
		  EditablePackingFormat::Unsigned8,
		  EditablePackingFormat::Signed4,
		  EditablePackingFormat::Unsigned4,
		  EditablePackingFormat::Boolean}) {
		EditablePacking policy;
		policy.Format = format;
		policy.Minimum = -1.0f;
		policy.Maximum = 1.0f;
		std::vector<std::byte> packed;
		std::vector<float> decoded;
		REQUIRE(PackEditableValues(values, policy, packed));
		REQUIRE(UnpackEditableValues(packed, values.size(), policy, decoded));
		REQUIRE(decoded.size() == values.size());
		const float bound =
			format == EditablePackingFormat::Boolean ? 1.0f
			: format == EditablePackingFormat::Signed4 || format == EditablePackingFormat::Unsigned4
				? 1.0f / 7.5f
			: format == EditablePackingFormat::Signed8 || format == EditablePackingFormat::Unsigned8
				? 1.0f / 127.5f
			: format == EditablePackingFormat::Float8E4M3FN ? 0.07f
															: 0.001f;
		for (size_t index = 0; index < values.size(); index++)
			CHECK(decoded[index] == Approx(values[index]).margin(bound));
	}
}

TEST_CASE("nibbles are low-first and booleans are least-significant-bit first", "[scene][editablepacking]") {
	EditablePacking policy;
	policy.Format = EditablePackingFormat::Unsigned4;
	std::vector<std::byte> packed;
	REQUIRE(PackEditableValues(std::array{0.0f, 1.0f, 0.5f}, policy, packed));
	REQUIRE(packed.size() == 2);
	CHECK(std::to_integer<uint8_t>(packed[0]) == 0xf0u);
	CHECK(std::to_integer<uint8_t>(packed[1]) == 0x08u);
	policy.Format = EditablePackingFormat::Boolean;
	REQUIRE(
		PackEditableValues(std::array{1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f}, policy, packed)
	);
	REQUIRE(packed.size() == 2);
	CHECK(std::to_integer<uint8_t>(packed[0]) == 0x8du);
	CHECK(std::to_integer<uint8_t>(packed[1]) == 0x01u);
}
