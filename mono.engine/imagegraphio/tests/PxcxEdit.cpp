#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

TEST_SUITE_ID("engine.imagegraphio.pxcxedit")
TEST_DEPENDS("engine.imagegraphio.pxcximport")

using namespace engine::imagegraph;
using namespace engine::imagegraphio;

namespace {
	PxcxImport
	Import(std::string_view input, bool grid = true, std::string_view type = "Node_Number_Simple") {
		engine::bake::PxcxArchive authored;
		authored.MetadataNumber = 121092;
		authored.MetadataText = "1.22.10.201";
		authored.GraphJson =
			R"JSON({"animator":{"frames_total":30,"playback":1,"framerate":30},"future":{"keep":[1,"two"]},"previewGrid":{"show":true,"snap":true,"size":[8,8],"opacity":0.5},"previewRuler":[],"nodes":[{"id":"number","type":)JSON" +
			std::string("\"") + std::string(type) +
			R"JSON(" ,"x":1,"y":2,"unknown":{"keep":17},"inputs":[)JSON" + std::string(input) +
			R"JSON(]},{"id":"opaque","type":"Unknown_Future_Node","x":3,"y":4,"group":"future-group","inputs":[],"opaque":{"keep":true}}]})JSON" +
			'\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(authored, bytes, failure));
		engine::bake::PxcxArchive archive;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure, {grid}));
		return imported;
	}
	PxcxImport Reimport(const std::vector<std::byte> &bytes, const PxcxImportOptions &options) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure, options));
		return result;
	}
	Value EvaluateNumber(const PxcxImport &imported, double frame = 0) {
		Document graph = imported.Graph;
		REQUIRE(graph.Groups.empty());
		REQUIRE(graph.SourceAnimators);
		CHECK(graph.SourceAnimators->Bindings.empty());
		REQUIRE(graph.SourceCommonOwners.size() == 2);
		const auto numberOwner = std::find_if(
			graph.SourceCommonOwners.begin(), graph.SourceCommonOwners.end(), [](const auto &owner) {
				return owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node &&
					   owner.NativeOwnerId == "number";
			}
		);
		REQUIRE(numberOwner != graph.SourceCommonOwners.end());
		const std::pair numberWriter{numberOwner->UpdateAnimatorOwnerId, numberOwner->UpdateAnimatorPort};
		const auto removedWriter = [&](std::string_view ownerId, std::string_view port) {
			return std::any_of(
				graph.SourceCommonOwners.begin(), graph.SourceCommonOwners.end(), [&](const auto &owner) {
					return owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node &&
						   owner.NativeOwnerId != "number" && owner.UpdateAnimatorOwnerId == ownerId &&
						   owner.UpdateAnimatorPort == port;
				}
			);
		};
		CHECK(
			std::count_if(
				graph.SourceCommonOwners.begin(), graph.SourceCommonOwners.end(), [](const auto &owner) {
					return owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node &&
						   owner.NativeOwnerId != "number";
				}
			) == 1
		);
		std::erase_if(graph.SourceAnimators->Detached, [&](const auto &row) {
			return removedWriter(row.OwnerId, row.Id);
		});
		std::erase_if(graph.SourceAnimators->DetachedValues, [&](const auto &row) {
			return removedWriter(row.NodeId, row.Port);
		});
		std::erase_if(graph.SourceCommonOwners, [](const auto &owner) {
			return owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node &&
				   owner.NativeOwnerId != "number";
		});
		REQUIRE(
			std::any_of(
				graph.SourceAnimators->Detached.begin(),
				graph.SourceAnimators->Detached.end(),
				[&](const auto &row) {
					return row.OwnerId == numberWriter.first && row.Id == numberWriter.second;
				}
			)
		);
		REQUIRE(
			std::any_of(
				graph.SourceAnimators->DetachedValues.begin(),
				graph.SourceAnimators->DetachedValues.end(),
				[&](const auto &row) {
					return row.NodeId == numberWriter.first && row.Port == numberWriter.second;
				}
			)
		);
		std::erase_if(graph.Nodes, [](const auto &node) { return node.Id != "number"; });
		REQUIRE(graph.Nodes.size() == 1);
		const auto *entry = FindCatalogueEntry(graph.Nodes.front().Type);
		REQUIRE(entry != nullptr);
		REQUIRE(entry->Outputs.size() == 1);
		graph.Outputs = {{"number", "number", std::string(entry->Outputs[0].Id)}};
		EvaluationRequest request;
		FrameTime time;
		REQUIRE(SplitFrameTime(frame, time));
		REQUIRE(SetFrameTime(request, time));
		Diagnostic diagnostic;
		EvaluatedValue value;
		Plan plan;
		REQUIRE(Compile(graph, plan, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValue(graph, plan, "number", request, value, diagnostic) == Status::Ok);
		return value.Data;
	}
	std::string Animated(std::string_view driver = "0") {
		return R"JSON({"anim":true,"on_end":0,"r":[[[0,2,"marker-tail"],0,[0,1],[0,0],0,0,true,)JSON" +
			   std::string(driver) +
			   R"JSON(,"key-tail"],[[1,12,0],10,[0,1],[0,0],0,0,true,0]],"future":{"keep":23}})JSON";
	}
}

TEST_CASE(
	"PXC edit transaction retains options unknown records and typed array values", "[imagegraphio][pxcx_edit]"
) {
	const auto imported =
		Import(R"JSON({"r":{"d":[1,2]},"future":{"keep":"input"}})JSON", false, "Node_Number");
	REQUIRE(imported.Graph.Nodes[0].Type == "pc.number");
	REQUIRE_FALSE(imported.Options.SavePreviewSettings);
	ArrayValue values{ValueType::Scalar, {3.0, 5.0, 8.0}};
	const std::array<PxcxEdit, 2> edits{
		PxcxInputValueEdit{"number", "value", values}, PxcxNodePositionEdit{"opaque", {-7.5, 11}}
	};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool written = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	const auto projected = Reimport(bytes, imported.Options);
	CHECK(std::get<ArrayValue>(EvaluateNumber(projected)) == values);
	CHECK(projected.Graph.Project->PreviewGrid == PreviewGridSettings{});
	for (const auto fragment :
		 {R"("future":{"keep":[1,"two"]})",
		  R"("opacity":0.5)",
		  R"("unknown":{"keep":17})",
		  R"("future":{"keep":"input"})",
		  R"("opaque":{"keep":true})",
		  R"("group":"future-group")"})
		CHECK(projected.Source.GraphJson.find(fragment) != std::string::npos);
	REQUIRE(projected.Source.Nodes.size() == 2);
	CHECK(projected.Source.Nodes[1].Id == "opaque");
	CHECK(projected.Source.Nodes[1].X == -7.5);
	CHECK(projected.Source.Nodes[1].Y == 11);
	CHECK(projected.Source.MetadataNumber == imported.Source.MetadataNumber);
	CHECK(projected.Source.MetadataText == imported.Source.MetadataText);
}

TEST_CASE(
	"PXC no-op and supplied unchanged edits preserve exact archive bytes", "[imagegraphio][pxcx_edit]"
) {
	const auto imported = Import(R"JSON({"r":{"d":4}})JSON");
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, {}, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
	const std::array<PxcxEdit, 2> unchanged{
		PxcxNodePositionEdit{"opaque", {3, 4}}, PxcxInputValueEdit{"number", "value", 4.0}
	};
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, unchanged, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
}

TEST_CASE(
	"PXC key replacements retain tails and edit kind time value and source ease", "[imagegraphio][pxcx_edit]"
) {
	const auto imported = Import(Animated());
	REQUIRE(imported.Graph.Keyframes.size() == 2);
	Keyframe replacement = imported.Graph.Keyframes[0];
	REQUIRE(SetFrameTime(replacement, FrameTime{1, .5, true}));
	replacement.Kind = KeyframeKind::Adder;
	replacement.Data = 4.0;
	replacement.Ease->In = {.25, .75};
	replacement.Ease->OutType = "cut";
	const std::array<PxcxEdit, 1> edits{
		PxcxKeyframeEdit{"number", "value", GetFrameTime(imported.Graph.Keyframes[0]), replacement}
	};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool written = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	const auto projected = Reimport(bytes, imported.Options);
	// A new archive rebases record identity; every executable key field must remain exact.
	auto expected = replacement;
	expected.SourceKeyId = projected.Graph.Keyframes[0].SourceKeyId;
	CHECK_FALSE(expected.SourceKeyId.empty());
	CHECK(expected.SourceKeyId != replacement.SourceKeyId);
	CHECK(projected.Graph.Keyframes[0] == expected);
	CHECK(std::get<double>(EvaluateNumber(projected, -.5)) == 4);
	CHECK(std::get<double>(EvaluateNumber(projected, 12)) == 10);
	CHECK(projected.Source.GraphJson.find(R"([1,-1.5,"marker-tail"])") != std::string::npos);
	CHECK(projected.Source.GraphJson.find(R"("key-tail")") != std::string::npos);
	CHECK(projected.Source.GraphJson.find(R"("future":{"keep":23})") != std::string::npos);
}

TEST_CASE(
	"PXC source driver controls update without dropping unknown driver fields", "[imagegraphio][pxcx_edit]"
) {
	const auto imported =
		Import(Animated(R"JSON({"typ":"sine","fre":0.25,"amp":2,"phs":0,"smt":0,"future":{"keep":9}})JSON"));
	Keyframe replacement = imported.Graph.Keyframes[0];
	std::get<KeyframeSineDriver>(*replacement.SourceDriver).Amplitude = 3;
	const std::array<PxcxEdit, 1> edits{
		PxcxKeyframeEdit{"number", "value", GetFrameTime(replacement), replacement}
	};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diagnostic));
	const auto projected = Reimport(bytes, imported.Options);
	CHECK(projected.Graph.Keyframes[0] == replacement);
	CHECK(projected.Source.GraphJson.find(R"("amp":3.0)") != std::string::npos);
	CHECK(projected.Source.GraphJson.find(R"("future":{"keep":9})") != std::string::npos);
	CHECK(
		std::get<double>(EvaluateNumber(projected, 3)) ==
		Catch::Approx(1 + 3 * std::sin(3 * .25 / 30 * 2 * std::numbers::pi))
	);
	Keyframe unsupported = replacement;
	unsupported.SourceDriver = KeyframeLinearDriver{2};
	const std::array<PxcxEdit, 1> loss{
		PxcxKeyframeEdit{"number", "value", GetFrameTime(replacement), unsupported}
	};
	std::vector<std::byte> sentinel{std::byte{0x77}};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, loss, sentinel, diagnostic));
	CHECK(sentinel == std::vector<std::byte>{std::byte{0x77}});
	CHECK(diagnostic.Message == "PXC driver edit would discard source metadata");
}

TEST_CASE("PXC stale ambiguous and unsupported edits refuse atomically", "[imagegraphio][pxcx_edit]") {
	const auto imported = Import(R"JSON({"r":{"d":4}})JSON");
	Diagnostic diagnostic;
	const std::vector<std::byte> initial{std::byte{0x77}};
	const std::array<PxcxEdit, 1> edit{PxcxInputValueEdit{"number", "value", 5.0}};
	auto bytes = initial;
	auto stale = imported.Source.OriginalBytes;
	stale[0] = std::byte{0};
	CHECK_FALSE(WritePxcxEdits(imported, stale, edit, bytes, diagnostic));
	CHECK(bytes == initial);
	CHECK(diagnostic.Message == "PXC edit source identity is stale");
	auto changed = imported;
	changed.Graph.Nodes[0].Position = {9, 9};
	CHECK_FALSE(WritePxcxEdits(changed, changed.Source.OriginalBytes, edit, bytes, diagnostic));
	CHECK(bytes == initial);
	CHECK(diagnostic.Message == "PXC edit projection has untracked changes");
	const std::array<PxcxEdit, 2> conflict{edit[0], edit[0]};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, conflict, bytes, diagnostic));
	CHECK(bytes == initial);
	const std::array<PxcxEdit, 1> opaque{PxcxInputValueEdit{"opaque", "value", 5.0}};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, opaque, bytes, diagnostic));
	CHECK(bytes == initial);
	const std::array<PxcxEdit, 1> nonfinite{
		PxcxInputValueEdit{"number", "value", std::numeric_limits<double>::infinity()}
	};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, nonfinite, bytes, diagnostic));
	CHECK(bytes == initial);
	const std::array<PxcxEdit, 1> lossyType{PxcxInputValueEdit{"number", "value", int64_t{5}}};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, lossyType, bytes, diagnostic));
	CHECK(bytes == initial);
	CHECK(diagnostic.Message == "PXC edited projection changed unsupported or ambiguous semantics");
}

TEST_CASE(
	"PXC key conflicts missing identities and unrepresentable time refuse atomically",
	"[imagegraphio][pxcx_edit]"
) {
	const auto imported = Import(Animated());
	const auto first = imported.Graph.Keyframes[0];
	Keyframe replacement = first;
	Diagnostic diagnostic;
	const std::vector<std::byte> initial{std::byte{0x77}};
	auto bytes = initial;
	REQUIRE(SetFrameTime(replacement, GetFrameTime(imported.Graph.Keyframes[1])));
	std::array<PxcxEdit, 1> edit{PxcxKeyframeEdit{"number", "value", GetFrameTime(first), replacement}};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edit, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC key edit would create an ambiguous timestamp");
	CHECK(bytes == initial);
	REQUIRE(SetFrameTime(replacement, FrameTime{13}));
	edit[0] = PxcxKeyframeEdit{"number", "value", GetFrameTime(first), replacement};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edit, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC key edit would reorder source records");
	CHECK(bytes == initial);
	edit[0] = PxcxKeyframeEdit{"number", "value", FrameTime{7}, first};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edit, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC original key identity is missing");
	CHECK(bytes == initial);
	REQUIRE(SetFrameTime(replacement, FrameTime{2, .1}));
	edit[0] = PxcxKeyframeEdit{"number", "value", GetFrameTime(first), replacement};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edit, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC key time cannot be represented exactly as a source real");
	CHECK(bytes == initial);
	const std::array<PxcxEdit, 1> unchanged{PxcxKeyframeEdit{"number", "value", GetFrameTime(first), first}};
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, unchanged, bytes, diagnostic));
	CHECK(bytes == imported.Source.OriginalBytes);
}

TEST_CASE("PXC aggregate edit payload limit refuses before source mutation", "[imagegraphio][pxcx_edit]") {
	const auto imported = Import(R"JSON({"r":{"d":4}})JSON");
	const std::vector<std::byte> initial{std::byte{0x77}};
	auto bytes = initial;
	Diagnostic diagnostic;
	// Each text is individually bounded. Their shared transaction budget is smaller than the total.
	std::vector<PxcxEdit> edits;
	const size_t count = Limits::MaximumArrayBytes / Limits::MaximumTextBytes + 1;
	for (size_t index = 0; index < count; ++index)
		edits.emplace_back(PxcxInputValueEdit{"number", "value", std::string(Limits::MaximumTextBytes, 'x')});
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC edit payload is invalid, unsupported or exceeds its aggregate limit");
	CHECK(bytes == initial);
}

TEST_CASE(
	"PXC verified driver inverse codecs retain source controls through archive replay",
	"[imagegraphio][pxcx_edit]"
) {
	const auto imported = Import(Animated());
	const auto first = imported.Graph.Keyframes[0];
	const std::array<KeyframeSourceDriver, 5> drivers{
		KeyframeLinearDriver{2},
		KeyframeSnapDriver{2},
		KeyframeBounceDriver{2, .4, 3},
		KeyframeElasticDriver{2, .4, 3},
		KeyframeCurveDriver{}
	};
	for (const auto &driver : drivers) {
		Keyframe replacement = first;
		replacement.SourceDriver = driver;
		const std::array<PxcxEdit, 1> edit{
			PxcxKeyframeEdit{"number", "value", GetFrameTime(first), replacement}
		};
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edit, bytes, diagnostic));
		const auto projected = Reimport(bytes, imported.Options);
		CHECK(projected.Graph.Keyframes[0] == replacement);
		CHECK(std::get<double>(EvaluateNumber(projected, 12)) == 10);
		if (std::holds_alternative<KeyframeLinearDriver>(driver))
			CHECK(std::get<double>(EvaluateNumber(projected, 3)) == 7);
		if (std::holds_alternative<KeyframeSnapDriver>(driver))
			CHECK(std::get<double>(EvaluateNumber(projected, 3)) == 0);
		replacement.SourceDriver.reset();
		const std::array<PxcxEdit, 1> remove{
			PxcxKeyframeEdit{"number", "value", GetFrameTime(replacement), replacement}
		};
		REQUIRE(WritePxcxEdits(projected, projected.Source.OriginalBytes, remove, bytes, diagnostic));
		CHECK(std::get<double>(EvaluateNumber(Reimport(bytes, imported.Options), 3)) == 1);
	}
}

TEST_CASE(
	"PXC edit transaction rejects changed source facts even when original bytes are unchanged",
	"[imagegraphio][pxcx_edit]"
) {
	auto imported = Import(R"JSON({"r":{"d":4}})JSON");
	imported.Source.GraphJson.insert(imported.Source.GraphJson.find("\"nodes\""), "\"future\":5,");
	const std::vector<std::byte> initial{std::byte{0x77}};
	auto bytes = initial;
	Diagnostic diagnostic;
	const std::array<PxcxEdit, 1> edit{PxcxNodePositionEdit{"opaque", {5, 6}}};
	CHECK_FALSE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edit, bytes, diagnostic));
	CHECK(bytes == initial);
	CHECK(diagnostic.Message.starts_with("PXC edit source is not a checked import:"));
}

TEST_CASE(
	"PXC matrix edits retain object fields and refuse ignored raw cells or inconsistent isize",
	"[imagegraphio][pxcx_edit]"
) {
	const MatrixValue matrix{2, 2, {1, 3, 2, 7}};
	const std::array<PxcxEdit, 1> edit{PxcxInputValueEdit{"number", "matrix", matrix}};
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const auto imported = Import(
		R"JSON({"r":{"d":{"size":[1,1],"isize":1,"raw":[4],"future":{"keep":19}}}})JSON",
		true,
		"Node_Matrix_Det"
	);
	REQUIRE(WritePxcxEdits(imported, imported.Source.OriginalBytes, edit, bytes, diagnostic));
	const auto projected = Reimport(bytes, imported.Options);
	CHECK(projected.Source.GraphJson.find(R"("future":{"keep":19})") != std::string::npos);
	CHECK(std::get<double>(EvaluateNumber(projected)) == 1);
	for (const auto input :
		 {R"JSON({"r":{"d":{"size":[1,1],"isize":1,"raw":[4,99]}}})JSON",
		  R"JSON({"r":{"d":{"size":[1,1],"isize":2,"raw":[4]}}})JSON",
		  R"JSON({"r":{"d":{"size":[1,1],"isize":1,"raw":[]}}})JSON"}) {
		const auto lossy = Import(input, true, "Node_Matrix_Det");
		const std::vector<std::byte> initial{std::byte{0x77}};
		bytes = initial;
		CHECK_FALSE(WritePxcxEdits(lossy, lossy.Source.OriginalBytes, edit, bytes, diagnostic));
		CHECK(bytes == initial);
		CHECK(diagnostic.Message == "PXC value inverse codec cannot retain source representation");
	}
}

TEST_CASE(
	"PXC projection compares track policies independently of source node order", "[imagegraphio][pxcx_edit]"
) {
	auto source = Import(R"JSON({"r":{"d":4}})JSON").Source;
	const auto type = source.GraphJson.find("Unknown_Future_Node");
	REQUIRE(type != std::string::npos);
	source.GraphJson.replace(type, std::string_view("Unknown_Future_Node").size(), "Node_Number_Simple");
	const auto group = source.GraphJson.find(",\"group\":\"future-group\"");
	REQUIRE(group != std::string::npos);
	source.GraphJson.erase(group, std::string_view(",\"group\":\"future-group\"").size());
	const auto inputs = source.GraphJson.find("\"inputs\":[]");
	REQUIRE(inputs != std::string::npos);
	source.GraphJson.replace(
		inputs, std::string_view("\"inputs\":[]").size(), R"JSON("inputs":[{"r":{"d":8}}])JSON"
	);
	source.Nodes.clear();
	source.Links.clear();
	std::vector<std::byte> original;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(source, original, failure));
	const auto imported = Reimport(original, {});
	auto authored = imported.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(authored, diagnostic) == Status::Ok);
	REQUIRE(authored.Tracks.size() == 2);
	std::reverse(authored.Tracks.begin(), authored.Tracks.end());
	const auto before = authored;
	std::vector<std::byte> written;
	const bool saved = WritePxcxProjection(imported, authored, {}, written, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	CHECK(written == original);
	CHECK(authored == before);
	authored.Tracks.push_back(authored.Tracks.front());
	CHECK_FALSE(WritePxcxProjection(imported, authored, {}, written, diagnostic));
	CHECK(written == original);
}
