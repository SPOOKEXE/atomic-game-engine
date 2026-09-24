#include "../src/Utf8TextOps.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.imagegraph.utf8_text_ops")

namespace ops = engine::imagegraph::detail;

TEST_CASE("Character and word counts preserve source text boundaries", "[imagegraph]") {
	int64_t length = -1;
	CHECK(ops::TextLength("A猫🙂B", 0, length) == ops::TextOpStatus::Ok);
	CHECK(length == 4);
	CHECK(ops::TextLength("", 1, length) == ops::TextOpStatus::Ok);
	CHECK(length == 1);
	CHECK(ops::TextLength("a  b ", 1, length) == ops::TextOpStatus::Ok);
	CHECK(length == 4);
	CHECK(ops::TextLength("abc", 2, length) == ops::TextOpStatus::InvalidMode);
}

TEST_CASE("Copy and delete use character positions across UTF-8 scalars", "[imagegraph]") {
	std::string output;
	CHECK(ops::CopyText("A猫🙂B", 2, 2, output) == ops::TextOpStatus::Ok);
	CHECK(output == "猫🙂");
	CHECK(ops::CopyText("A猫🙂B", 9, 2, output) == ops::TextOpStatus::Ok);
	CHECK(output.empty());
	CHECK(ops::CopyText("abc", -1, 1, output) == ops::TextOpStatus::UnsupportedIndex);
	CHECK(ops::CopyText("abc", 1, -1, output) == ops::TextOpStatus::UnsupportedIndex);
	CHECK(ops::DeleteText("A猫🙂B", 1, 2, output) == ops::TextOpStatus::Ok);
	CHECK(output == "AB");
	CHECK(ops::DeleteText("A猫🙂B", -2, 1, output) == ops::TextOpStatus::Ok);
	CHECK(output == "A猫🙂");
	CHECK(ops::DeleteText("A猫🙂B", 2, -2, output) == ops::TextOpStatus::Ok);
	CHECK(output == "AB");
}

TEST_CASE("Malformed UTF-8 and oversized text fail before output allocation", "[imagegraph]") {
	std::string output;
	int64_t length = -1;
	CHECK(ops::TextLength("\xC0\xAF", 0, length) == ops::TextOpStatus::InvalidUtf8);
	CHECK(ops::CopyText("\xED\xA0\x80", 1, 1, output) == ops::TextOpStatus::InvalidUtf8);
	CHECK(ops::DeleteText("\xF4\x90\x80\x80", 0, 1, output) == ops::TextOpStatus::InvalidUtf8);
	const std::string oversized(engine::imagegraph::Limits::MaximumTextBytes + 1, 'a');
	CHECK(ops::TextLength(oversized, 0, length) == ops::TextOpStatus::LimitExceeded);
}
