#include "NodeExecutors.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceCommonDispatch.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.source_common_dispatch")
using namespace engine::imagegraph;

TEST_CASE(
	"Common source wrapper admission distinguishes Number Lite from processor Full",
	"[source_common_dispatch]"
) {
	CHECK(
		SourceCommonDispatchProfile("Node_Number", true) ==
		SourceCommonDispatch{SourceCommonStepKind::NodeDataCommon, SourceCommonWrapperKind::Lite}
	);
	for (auto source : {"Node_Boolean", "Node_String", "Node_Solid"}) {
		INFO(source);
		CHECK(
			SourceCommonDispatchProfile(source, true) ==
			SourceCommonDispatch{SourceCommonStepKind::NodeDataCommon, SourceCommonWrapperKind::Full}
		);
	}
}
TEST_CASE(
	"Inherited group and input dispatch preserve overridden source methods", "[source_common_dispatch]"
) {
	for (auto source : {"Node_Collection", "Node_Group", "Node_Pixel_Builder"}) {
		INFO(source);
		CHECK(
			SourceCommonDispatchProfile(source, true) ==
			SourceCommonDispatch{SourceCommonStepKind::CollectionOverride, SourceCommonWrapperKind::Lite}
		);
	}
	CHECK(
		SourceCommonDispatchProfile("Node_Iterate_Each_Inline", true) ==
		SourceCommonDispatch{SourceCommonStepKind::NodeDataCommon, SourceCommonWrapperKind::Full}
	);
	for (auto source : {"Node_Group_Input", "Node_Feedback_Input", "Node_Iterator_Input"}) {
		INFO(source);
		CHECK(
			SourceCommonDispatchProfile(source, true) ==
			SourceCommonDispatch{SourceCommonStepKind::NodeDataCommon, SourceCommonWrapperKind::Lite}
		);
	}
}
TEST_CASE(
	"Custom empty wrappers admit only exact annotations without Full dispatch", "[source_common_dispatch]"
) {
	for (auto source : {"Node_Frame", "Node_Display_Text"}) {
		INFO(source);
		const auto profile = SourceCommonDispatchProfile(source, true);
		CHECK(profile.Step == SourceCommonStepKind::NodeDataCommon);
		CHECK(profile.Wrapper == SourceCommonWrapperKind::Empty);
		CHECK(SourceCommonDispatchProfile(source, false) == profile);
	}
	CHECK(SourceCommonDispatchProfile("Node_Number") == SourceCommonDispatch{});
	CHECK(SourceCommonDispatchProfile("Node_Number", false) == SourceCommonDispatch{});
	CHECK(SourceCommonDispatchProfile("Node_Solid", false) == SourceCommonDispatch{});
	CHECK(SourceCommonDispatchProfile("Node_Not_Captured", true) == SourceCommonDispatch{});
	CHECK(SourceCommonDispatchProfile("pc.number", true) == SourceCommonDispatch{});
	CHECK(SourceCommonDispatchProfile({}, true) == SourceCommonDispatch{});
	CHECK(
		SourceCommonDispatchProfile("Node_Collection") ==
		SourceCommonDispatch{SourceCommonStepKind::CollectionOverride, SourceCommonWrapperKind::Lite}
	);
}
TEST_CASE(
	"Actual registered native capability gates every catalogue source profile", "[source_common_dispatch]"
) {
	size_t native = 0, sourceOnly = 0;
	for (const auto &entry : Catalogue()) {
		INFO(entry.SourceNode);
		const bool executable = detail::FindExecutor(entry.Type) != nullptr;
		const auto profile = SourceCommonDispatchProfile(entry.SourceNode, executable);
		if (executable) {
			++native;
			CHECK(profile.Step != SourceCommonStepKind::Unsupported);
		} else {
			++sourceOnly;
			if (entry.SourceNode == "Node_Frame" || entry.SourceNode == "Node_Display_Text")
				CHECK(profile.Wrapper == SourceCommonWrapperKind::Empty);
			else
				CHECK(profile == SourceCommonDispatch{});
		}
	}
	CHECK(native > 0);
	CHECK(sourceOnly > 0);
}
