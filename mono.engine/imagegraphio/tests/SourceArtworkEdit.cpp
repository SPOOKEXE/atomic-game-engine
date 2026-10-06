#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/imagegraphio/SourceArtworkEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.source_artwork_edit")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	StructValue Object(std::vector<std::pair<std::string, Value>> fields) {
		StructValue object;
		object.Data.emplace();
		object.Data->Fields = std::move(fields);
		return object;
	}
	ArrayValue Objects(std::initializer_list<StructValue> values, bool inspection = false) {
		ArrayValue array;
		array.ElementType = inspection ? ValueType::Any : ValueType::Struct;
		for (const auto &value : values) {
			if (inspection)
				array.Items.push_back({ElementValue{value}});
			else
				array.Elements.emplace_back(value);
		}
		return array;
	}
	PxcxImport FileSource(std::string sourceType, Json extraNodes = Json::array()) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson =
			Json{
				{"animator", {{"frames_total", 12}, {"playback", 0}, {"framerate", 30}}},
				{"nodes",
				 Json::array({Json{
					 {"id", "file"},
					 {"type", sourceType},
					 {"name", "File"},
					 {"iname", "Original_File"},
					 {"x", 20},
					 {"y", 100},
					 {"inputs", Json::array({Json{{"r", {{"d", "artwork"}}}}})},
					 {"future_file", "retained"}
				 }})},
				{"future_project", 41}
			}
				.dump() +
			'\0';
		const size_t extraCount = extraNodes.size();
		if (!extraNodes.empty()) {
			auto graph = Json::parse(source.GraphJson.c_str());
			for (auto &node : extraNodes)
				graph["nodes"].push_back(std::move(node));
			source.GraphJson = graph.dump() + '\0';
		}
		std::string failure;
		std::vector<std::byte> bytes;
		const bool written = engine::bake::WritePxcx(source, bytes, failure);
		INFO(failure);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 1 + extraCount);
		return imported;
	}
	GroupReplayState Bound(const Document &document) {
		GroupReplayState empty, initialized, bound;
		Diagnostic diagnostic;
		REQUIRE(RebindGroupReplay(document, empty, 7, initialized, diagnostic) == Status::Ok);
		REQUIRE(BindGroupReplay(document, {}, initialized, 7, bound, diagnostic) == Status::Ok);
		return bound;
	}
	HostNodeCapture Layers(const Node &node) {
		HostNodeCapture capture;
		capture.Authored = node;
		capture.Tick = 3;
		capture.Subframe = .5;
		capture.NegativeFrame = true;
		capture.Outputs.push_back(
			{"content",
			 Object(
				 {{"layerData",
				   Objects(
					   {Object({{"name", std::string("Ink layer")}}),
						Object({{"name", std::string("Paint")}})}
				   )}}
			 )}
		);
		return capture;
	}
	HostNodeCapture Ase(const Node &node) {
		HostNodeCapture capture;
		capture.Authored = node;
		const auto layer = [](std::string name, int64_t type) {
			return Object({{"Type", int64_t{0x2004}}, {"Name", std::move(name)}, {"Layer type", type}});
		};
		const auto tag = [](std::string name, int64_t first, int64_t last, int64_t colour) {
			return Object(
				{{"Name", std::move(name)}, {"Frame start", first}, {"Frame end", last}, {"Color", colour}}
			);
		};
		capture.Outputs.push_back(
			{"content",
			 Object(
				 {{"Frame amount", int64_t{2}},
				  {"Frames",
				   Objects(
					   {Object(
							{{"Chunks",
							  Objects(
								  {layer("Folder", 1),
								   layer("Ink", 0),
								   layer("Ink", 0),
								   layer("Ink_1", 0),
								   Object(
									   {{"Type", int64_t{0x2018}},
										{"Tags", Objects({tag("discarded", 0, 0, 0)}, true)}}
								   ),
								   Object(
									   {{"Type", int64_t{0x2018}},
										{"Tags",
										 Objects(
											 {tag("Run", 0, 1, 0x563412), tag("Run", 1, 0, 0xffffff)}, true
										 )}}
								   )},
								  true
							  )}}
						),
						Object(
							{{"Chunks",
							  Objects(
								  {Object({{"Type", int64_t{0x2005}}, {"Layer index", int64_t{1}}}),
								   Object(
									   {{"Type", int64_t{0x2018}},
										{"Tags", Objects({tag("ignored_later_frame", 0, 0, 0)}, true)}}
								   )},
								  true
							  )}}
						)},
					   true
				   )}}
			 )}
		);
		return capture;
	}
}
TEST_CASE(
	"Prepared ASE callbacks retain pinned duplicate names tag chunk and cel loop behavior",
	"[imagegraphio][artwork]"
) {
	const auto imported = FileSource("Node_ASE_File_Read");
	const auto capture = Ase(imported.Graph.Nodes[0]);
	SourceArtworkMetadata metadata;
	Diagnostic diagnostic;
	const auto status = ReadSourceArtworkMetadata(capture, metadata, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(metadata.Layers.size() == 4);
	CHECK_FALSE(metadata.Layers[0].Renderable);
	CHECK(metadata.Layers[1].Name == "Ink");
	CHECK(metadata.Layers[2].Name == "Ink_1");
	CHECK(metadata.Layers[3].Name == "Ink_1");
	CHECK_FALSE(metadata.Layers[1].Loop);
	CHECK(metadata.Layers[2].Loop);
	REQUIRE(metadata.Tags.size() == 2);
	CHECK(metadata.Tags[0].Name == "Run");
	CHECK(metadata.Tags[1].Name == "Run_1");
	CHECK(metadata.Tags[0].Color == Colour{0x12, 0x34, 0x56, 255});
	CHECK(metadata.Tags[1].First == 1);
	CHECK(metadata.Tags[1].Last == 0);
	const auto names = metadata.Layers;
	CHECK(ReadSourceArtworkMetadata(capture, metadata, diagnostic, 1) == Status::LimitExceeded);
	CHECK(metadata.Layers[1].Name == names[1].Name);
}
TEST_CASE(
	"Generate artwork layers is one source-preserving prepared authoring transaction",
	"[imagegraphio][artwork]"
) {
	for (const auto *sourceType : {"Node_ORA_File_Read", "Node_Krita_File_Read"}) {
		CAPTURE(sourceType);
		const auto imported = FileSource(sourceType);
		const auto capture = Layers(imported.Graph.Nodes[0]);
		const auto replay = Bound(imported.Graph);
		Document candidate;
		GroupReplayState candidateReplay;
		Diagnostic diagnostic;
		const auto status = ApplySourceArtworkEdit(
			imported.Graph,
			capture,
			replay,
			{SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
			candidate,
			candidateReplay,
			diagnostic
		);
		INFO(diagnostic.NodeId);
		INFO(diagnostic.Port);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(candidate.Nodes.size() == 3);
		REQUIRE(candidate.Links.size() == 2);
		CHECK(candidate.Nodes[1].SourceDisplayName == "Ink layer");
		CHECK(candidate.Nodes[1].SourceInternalName == "Ink_layer");
		CHECK(candidate.Nodes[1].Position == Vector2{180, 68});
		CHECK(candidate.Nodes[2].Position == Vector2{180, 132});
		Document executable = candidate;
		const auto *entry = FindCatalogueEntry(candidate.Nodes[1].Type);
		REQUIRE(entry);
		const auto output = std::find_if(entry->Outputs.begin(), entry->Outputs.end(), [](const auto &port) {
			return port.SourceIndex == 0;
		});
		REQUIRE(output != entry->Outputs.end());
		executable.Outputs = {{"layer-preview", candidate.Nodes[1].Id, std::string(output->Id)}};
		Plan plan;
		const auto compiled = Compile(executable, plan, diagnostic);
		INFO(diagnostic.NodeId);
		INFO(diagnostic.Port);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		CHECK(imported.Graph.Nodes.size() == 1);
		std::vector<std::byte> bytes;
		const auto saved = WritePxcxProjection(imported, candidate, {3, .5, true}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(saved);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport reloaded;
		REQUIRE(ImportPxcxImageGraph(checked, reloaded, failure));
		CHECK(reloaded.Graph.Nodes.size() == 3);
		CHECK(reloaded.Graph.Links == candidate.Links);
		CHECK(Json::parse(checked.GraphJson.c_str())["nodes"][0]["future_file"] == "retained");
		std::vector<std::byte> noOp;
		REQUIRE(WritePxcxProjection(reloaded, reloaded.Graph, {}, noOp, diagnostic));
		CHECK(noOp == bytes);
	}
}
TEST_CASE(
	"ASE Match Frames and Import Tags preserve source regions and original owners", "[imagegraphio][artwork]"
) {
	const auto imported = FileSource("Node_ASE_File_Read");
	const auto capture = Ase(imported.Graph.Nodes[0]);
	const auto replay = Bound(imported.Graph);
	Diagnostic diagnostic;
	Document candidate;
	GroupReplayState candidateReplay;
	REQUIRE(
		ApplySourceArtworkEdit(
			imported.Graph,
			capture,
			replay,
			{SourceArtworkAction::MatchFrames, true, true, 7, 8, std::nullopt},
			candidate,
			candidateReplay,
			diagnostic
		) == Status::Ok
	);
	CHECK(candidate.Timeline->Frames == 2);
	CHECK(candidate.Timeline->Last == 1);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, candidate, {}, bytes, diagnostic));
	REQUIRE(
		ApplySourceArtworkEdit(
			imported.Graph,
			capture,
			replay,
			{SourceArtworkAction::ImportTags, true, true, 7, 8, std::nullopt},
			candidate,
			candidateReplay,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(candidate.Project);
	REQUIRE(candidate.Project->AnimationRegions.size() == 2);
	CHECK(candidate.Project->AnimationRegions[0].Label == "Run");
	CHECK(candidate.Project->AnimationRegions[0].Start == FrameTime{1, 0, false});
	CHECK(candidate.Project->AnimationRegions[0].End == FrameTime{2, 0, false});
	CHECK(candidate.Project->AnimationRegions[1].Start == FrameTime{2, 0, false});
	CHECK(candidate.Project->AnimationRegions[1].End == FrameTime{1, 0, false});
	CHECK(imported.Graph.Timeline->Frames == 12);
	const auto prior = candidate;
	auto stale = capture;
	stale.Authored.Position.X += 1;
	CHECK(
		ApplySourceArtworkEdit(
			imported.Graph,
			stale,
			replay,
			{SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
			candidate,
			candidateReplay,
			diagnostic
		) == Status::InvalidValue
	);
	CHECK(candidate == prior);
}
TEST_CASE(
	"Reused artwork setters preserve physical key provenance and shared owner bindings",
	"[imagegraphio][artwork]"
) {
	const auto record = [](double frame, std::string text) {
		return Json::array(
			{Json::array({0, frame}),
			 std::move(text),
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 4294967295u,
			 "opaque-key-tail"}
		);
	};
	const auto layer = [&](std::string id, std::string name, std::string base) {
		Json node{
			{"id", id},
			{"type", "Node_ORA_layer"},
			{"name", name},
			{"iname", "retained_" + id},
			{"x", 900},
			{"y", 901},
			{"inputs",
			 Json::array(
				 {Json{{"from_node", "file"}, {"from_index", 1}, {"future_data", 71}},
				  Json{
					  {"anim", true},
					  {"r",
					   base.empty() ? Json::array({record(0, "Before"), record(8, "After")}) : Json::array()},
					  {"future_control", 73}
				  }}
			 )},
			{"future_layer", 79}
		};
		if (!base.empty()) node["instanceBase"] = base;
		return node;
	};
	const auto imported = FileSource(
		"Node_ORA_File_Read",
		Json::array(
			{layer("base", "Base", ""),
			 layer("copy", "Ink layer", "base"),
			 layer("sibling", "Sibling", "base")}
		)
	);
	REQUIRE(imported.Graph.Groups.empty());
	REQUIRE(imported.Graph.Nodes.size() == 4);
	std::string importNotes;
	for (const auto &note : imported.Diagnostics)
		importNotes += note.NodeId + ":" + note.Message + "\n";
	INFO(importNotes);
	INFO(imported.Graph.Nodes[2].Type);
	INFO(imported.Graph.Nodes[3].Type);
	CHECK(imported.Graph.Nodes[2].InstanceBase == "base");
	CHECK(imported.Graph.Nodes[3].InstanceBase == "base");
	for (size_t i : {size_t{2}, size_t{3}}) {
		CHECK(imported.Graph.Nodes[i].Type == "pc.ora_layer");
		CHECK(
			std::none_of(
				imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == imported.Graph.Nodes[i].Id;
				}
			)
		);
	}

	Diagnostic diagnostic;
	GroupReplayState empty, initialized, bound;
	REQUIRE(RebindGroupReplay(imported.Graph, empty, 7, initialized, diagnostic) == Status::Ok);
	const GroupSubtypeBinding bindings[] = {
		{"copy", "base", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "layer_name"},
		{"sibling", "base", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "layer_name"}
	};
	const auto boundStatus = BindGroupReplay(imported.Graph, bindings, initialized, 7, bound, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(boundStatus == Status::Ok);
	const auto capture = Layers(imported.Graph.Nodes[0]);
	Document candidate;
	GroupReplayState resultReplay;
	const auto status = ApplySourceArtworkEdit(
		imported.Graph,
		capture,
		bound,
		{SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
		candidate,
		resultReplay,
		diagnostic
	);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(candidate.Nodes.size() == 5);
	const auto copy = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [](const auto &node) {
		return node.Id == "copy";
	});
	REQUIRE(copy != candidate.Nodes.end());
	CHECK(copy->Position == Vector2{900, 901});
	CHECK(copy->SourceInternalName == "retained_copy");
	CHECK(copy->InstanceBase == "base");
	for (const auto &old : imported.Graph.Keyframes) {
		const auto key =
			std::find_if(candidate.Keyframes.begin(), candidate.Keyframes.end(), [&](const auto &item) {
				return item.SourceKeyId == old.SourceKeyId;
			});
		REQUIRE(key != candidate.Keyframes.end());
		CHECK(*key == old);
	}
	const auto inserted =
		std::find_if(candidate.Keyframes.begin(), candidate.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "base" && key.Port == "layer_name" && key.NegativeFrame && key.Tick == 3 &&
				   key.Subframe == .5;
		});
	REQUIRE(inserted != candidate.Keyframes.end());
	CHECK(inserted->Data == Value{std::string("Ink layer")});
	CHECK(resultReplay.Binding("copy", "layer_name")->OwnerId == "base");
	CHECK(resultReplay.Binding("sibling", "layer_name")->OwnerId == "base");
	CHECK(resultReplay.AuthoringRevision() == 8);
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, candidate, {3, .5, true}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive checked;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
	const auto graph = Json::parse(checked.GraphJson.c_str());
	CHECK(graph["nodes"][2]["iname"] == "retained_copy");
	CHECK(graph["nodes"][2]["inputs"][1]["future_control"] == 73);
	CHECK(graph["nodes"][2]["inputs"][1]["r"] == Json::array());
	CHECK(graph["nodes"][3]["inputs"][1]["r"] == Json::array());
	REQUIRE(candidate.SourceAnimators);
	auto unreconstructable = candidate;
	unreconstructable.SourceAnimators->Bindings.front().AnimatorPort = "layer_index";
	const auto retainedBytes = bytes;
	CHECK_FALSE(WritePxcxProjection(imported, unreconstructable, {3, .5, true}, bytes, diagnostic));
	CHECK(diagnostic.Message.find("cannot reconstruct captured animator generations") != std::string::npos);
	CHECK(bytes == retainedBytes);
	unreconstructable = candidate;
	auto &captured = unreconstructable.SourceAnimators->Bindings.front();
	captured.Writer = captured.Writer == GroupSubtypeAnimator::Animated ? GroupSubtypeAnimator::Static
																		: GroupSubtypeAnimator::Animated;
	CHECK_FALSE(WritePxcxProjection(imported, unreconstructable, {3, .5, true}, bytes, diagnostic));
	CHECK(diagnostic.Message.find("cannot reconstruct captured animator generations") != std::string::npos);
	CHECK(bytes == retainedBytes);

	CHECK(graph["nodes"][1]["inputs"][1]["r"][1].back() == "opaque-key-tail");
	CHECK(bound.AuthoringRevision() == 7);
}

TEST_CASE(
	"Generate Layers refuses a source-reused incompatible setter without partial authoring",
	"[imagegraphio][artwork]"
) {
	const auto consumer = Json{
		{"id", "existing"},
		{"x", 900},
		{"y", 901},
		{"type", "Node_ASE_layer"},
		{"name", "Paint"},
		{"iname", "KeptConsumer"},
		{"inputs",
		 Json::array(
			 {Json{{"from_node", "file"}, {"from_index", 1}},
			  Json{{"r", {{"d", false}}}},
			  Json{{"r", {{"d", "Paint"}}}}}
		 )},
		{"future_consumer", 99}
	};
	const auto imported = FileSource("Node_ORA_File_Read", Json::array({consumer}));
	const auto receipt = Layers(imported.Graph.Nodes[0]);
	const auto replay = Bound(imported.Graph);
	Document output = imported.Graph;
	GroupReplayState outputReplay = Bound(imported.Graph);
	const auto prior = output;
	const auto priorReplay = outputReplay.RetainedBytes();
	Diagnostic diagnostic;
	CHECK(
		ApplySourceArtworkEdit(
			imported.Graph,
			receipt,
			replay,
			{SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
			output,
			outputReplay,
			diagnostic
		) == Status::UnsupportedExecution
	);
	CHECK(diagnostic.Message.find("setters") != std::string::npos);
	CHECK(output == prior);
	CHECK(outputReplay.AuthoringRevision() == 7);
	CHECK(outputReplay.RetainedBytes() == priorReplay);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, output, {}, bytes, diagnostic));
	std::vector<std::byte> original;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, original, diagnostic));
	CHECK(bytes == original);
}
TEST_CASE(
	"Import Tags matches the last equal region or appends every tag on explicit shift action",
	"[imagegraphio][artwork]"
) {
	auto imported = FileSource("Node_ASE_File_Read");
	imported.Graph.Project.emplace();
	imported.Graph.Project->AnimationRegions = {
		{"Run", Colour{1, 2, 3, 4}, {9, 0, false}, {10, 0, false}},
		{"Other", Colour{5, 6, 7, 8}, {3, 0, false}, {4, 0, false}},
		{"Run", Colour{9, 10, 11, 12}, {5, 0, false}, {6, 0, false}}
	};
	const auto receipt = Ase(imported.Graph.Nodes[0]);
	const auto replay = Bound(imported.Graph);
	Document matched, appended;
	GroupReplayState matchedReplay, appendedReplay;
	Diagnostic diagnostic;
	REQUIRE(
		ApplySourceArtworkEdit(
			imported.Graph,
			receipt,
			replay,
			{SourceArtworkAction::ImportTags, true, true, 7, 8, std::nullopt},
			matched,
			matchedReplay,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(matched.Project->AnimationRegions.size() == 4);
	CHECK(matched.Project->AnimationRegions[0] == imported.Graph.Project->AnimationRegions[0]);
	CHECK(matched.Project->AnimationRegions[1] == imported.Graph.Project->AnimationRegions[1]);
	CHECK(matched.Project->AnimationRegions[2].Start == FrameTime{1, 0, false});
	CHECK(matched.Project->AnimationRegions[2].Color == Colour{0x12, 0x34, 0x56, 255});
	CHECK(matched.Project->AnimationRegions[3].Label == "Run_1");
	REQUIRE(
		ApplySourceArtworkEdit(
			imported.Graph,
			receipt,
			replay,
			{SourceArtworkAction::ImportTags, false, true, 7, 8, std::nullopt},
			appended,
			appendedReplay,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(appended.Project->AnimationRegions.size() == 5);
	CHECK(appended.Project->AnimationRegions[2] == imported.Graph.Project->AnimationRegions[2]);
	CHECK(appended.Project->AnimationRegions[3].Label == "Run");
	CHECK(appended.Project->AnimationRegions[4].Label == "Run_1");
}

TEST_CASE(
	"ASE Generate Layers skips group layers and reuses native source suffix collisions",
	"[imagegraphio][artwork]"
) {
	const auto imported = FileSource("Node_ASE_File_Read");
	auto receipt = Ase(imported.Graph.Nodes[0]);
	const auto *fileEntry = FindCatalogueEntry(imported.Graph.Nodes[0].Type);
	REQUIRE(fileEntry);
	const auto crop = std::find_if(fileEntry->Inputs.begin(), fileEntry->Inputs.end(), [](const auto &input) {
		return input.SourceIndex == 3;
	});
	REQUIRE(crop != fileEntry->Inputs.end());
	receipt.Inputs.push_back({std::string(crop->Id), true});
	const auto replay = Bound(imported.Graph);
	Document result;
	GroupReplayState resultReplay;
	Diagnostic diagnostic;
	const auto status = ApplySourceArtworkEdit(
		imported.Graph,
		receipt,
		replay,
		{SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
		result,
		resultReplay,
		diagnostic
	);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Nodes.size() == 3);
	REQUIRE(result.Links.size() == 2);
	CHECK(result.Nodes[1].SourceDisplayName == "Ink");
	CHECK(result.Nodes[1].Position == Vector2{180, 68});
	CHECK(result.Nodes[2].SourceDisplayName == "Ink_1");
	CHECK(result.Nodes[2].Position == Vector2{180, 132});
	for (size_t i = 1; i < result.Nodes.size(); ++i) {
		const auto &node = result.Nodes[i];
		const auto value = [&](std::string_view port) -> const Value * {
			for (const auto &control : node.Values)
				if (control.Port == port) return &control.Data;
			return nullptr;
		};
		REQUIRE(node.Values.size() == 4);
		CHECK(node.Values[0].Port == "crop_output");
		CHECK(node.Values[1].Port == "layer_name");
		CHECK(
			node.SourceStaticInputs ==
			std::vector<std::string>{"ase_data", "layer_name", "crop_output", "loop", "apply_opacity"}
		);
		REQUIRE(value("crop_output"));
		CHECK(std::get<bool>(*value("crop_output")));
		REQUIRE(value("loop"));
		CHECK(std::get<bool>(*value("loop")) == (i == 2));
		REQUIRE(value("apply_opacity"));
		CHECK(std::get<bool>(*value("apply_opacity")));
	}
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, result, {}, bytes, diagnostic);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport reloaded;
	REQUIRE(ImportPxcxImageGraph(archive, reloaded, failure));
	CHECK(reloaded.Graph.Links == result.Links);
	REQUIRE(reloaded.Graph.Nodes.size() == 3);
	CHECK(reloaded.Graph.Nodes[2].Position == result.Nodes[2].Position);
}

TEST_CASE(
	"ASE Match Frames preserves explicit saved endpoint independently of new total",
	"[imagegraphio][artwork][source_timeline]"
) {
	const auto initial = FileSource("Node_ASE_File_Read");
	auto source = initial.Source;
	auto json = Json::parse(source.GraphJson.c_str());
	json["animator"]["frame_range_end"] = 12;
	json["animator"]["future_bounds"] = "retained";
	source.GraphJson = json.dump() + '\0';
	std::vector<std::byte> archiveBytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(source, archiveBytes, failure));
	engine::bake::PxcxArchive checked;
	REQUIRE(engine::bake::ReadPxcx(archiveBytes, checked, failure));
	PxcxImport imported;
	REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
	REQUIRE(imported.Graph.Timeline);
	REQUIRE(imported.Graph.Timeline->SourceBounds);
	const auto receipt = Ase(imported.Graph.Nodes[0]);
	const auto replay = Bound(imported.Graph);
	Document result;
	GroupReplayState resultReplay;
	Diagnostic diagnostic;
	REQUIRE(
		ApplySourceArtworkEdit(
			imported.Graph,
			receipt,
			replay,
			{SourceArtworkAction::MatchFrames, true, true, 7, 8, std::nullopt},
			result,
			resultReplay,
			diagnostic
		) == Status::Ok
	);
	CHECK(result.Timeline->Frames == 2);
	CHECK(result.Timeline->Last == 1);
	CHECK(result.Timeline->SourceBounds == imported.Graph.Timeline->SourceBounds);
	CHECK(SourceTimelineLastFrame(*result.Timeline) == 11.);
	REQUIRE(WritePxcxProjection(imported, result, {}, archiveBytes, diagnostic));
	REQUIRE(engine::bake::ReadPxcx(archiveBytes, checked, failure));
	PxcxImport reloaded;
	REQUIRE(ImportPxcxImageGraph(checked, reloaded, failure));
	CHECK(reloaded.Graph.Timeline == result.Timeline);
	const auto saved = Json::parse(checked.GraphJson.c_str());
	CHECK(saved["animator"]["frames_total"] == 2);
	CHECK(saved["animator"]["frame_range_end"] == 12);
	CHECK(saved["animator"]["future_bounds"] == "retained");
}

TEST_CASE(
	"ASE name normalization admits text comparison work before publishing metadata", "[imagegraphio][artwork]"
) {
	Node file;
	file.Id = "file";
	file.Type = "pc.ase_file_read";
	ArrayValue chunks;
	chunks.ElementType = ValueType::Struct;
	const std::string name(8192, 'x');
	for (size_t i = 0; i < 66; ++i)
		chunks.Elements.emplace_back(
			Object({{"Type", int64_t{0x2004}}, {"Name", name}, {"Layer type", int64_t{0}}})
		);
	const Value content = Object(
		{{"Frame amount", int64_t{1}}, {"Frames", Objects({Object({{"Chunks", std::move(chunks)}})})}}
	);
	SourceArtworkMetadata metadata;
	metadata.Frames = 47;
	metadata.Layers.push_back({"prior", false, false});
	metadata.Tags.push_back({"retained", {3, 4, 5, 6}, 7, 8});
	const auto layersCapacity = metadata.Layers.capacity();
	const auto tagsCapacity = metadata.Tags.capacity();
	Diagnostic diagnostic;
	CHECK(ReadSourceAsepriteMetadata(file, content, metadata, diagnostic) == Status::LimitExceeded);
	CHECK(diagnostic.Message.find("comparison-work") != std::string::npos);
	REQUIRE(metadata.Layers.size() == 1);
	CHECK(metadata.Layers[0].Name == "prior");
	CHECK(metadata.Layers.capacity() == layersCapacity);
	REQUIRE(metadata.Tags.size() == 1);
	CHECK(metadata.Tags[0].Name == "retained");
	CHECK(metadata.Tags.capacity() == tagsCapacity);
	CHECK(metadata.Frames == 47);
}

TEST_CASE(
	"Artwork transaction accounts retained prior document and animator owners together",
	"[imagegraphio][artwork]"
) {
	const auto imported = FileSource("Node_ORA_File_Read");
	const auto capture = Layers(imported.Graph.Nodes[0]);
	const auto replay = Bound(imported.Graph);
	Document output = imported.Graph;
	GroupReplayState outputReplay = Bound(imported.Graph);
	const auto previousBytes = DocumentRetainedPayloadBytes(output);
	const auto previousReplayBytes = outputReplay.RetainedBytes();
	REQUIRE(previousBytes);
	const auto authoredBytes = DocumentRetainedPayloadBytes(imported.Graph);
	const auto captureBytes = HostCaptureRetainedPayloadBytes(capture);
	REQUIRE(authoredBytes);
	REQUIRE(captureBytes);
	Diagnostic diagnostic;
	const uint64_t cap =
		*authoredBytes + *previousBytes + *captureBytes + replay.RetainedBytes() + previousReplayBytes;
	CHECK(
		ApplySourceArtworkEdit(
			imported.Graph,
			capture,
			replay,
			{SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
			output,
			outputReplay,
			diagnostic,
			cap
		) == Status::LimitExceeded
	);
	CHECK(output == imported.Graph);
	CHECK(outputReplay.RetainedBytes() == previousReplayBytes);
	CHECK(outputReplay.AuthoringRevision() == 7);
}

TEST_CASE(
	"Generate Layers cannot reuse a consumer removed by the native source activity projection",
	"[imagegraphio][artwork]"
) {
	const Json consumer{
		{"id", "removed"},
		{"x", 900},
		{"y", 901},
		{"type", "Node_ORA_layer"},
		{"name", "Ink layer"},
		{"iname", "removed_original"},
		{"inputs", Json::array({Json{{"from_node", "file"}, {"from_index", 1}}, Json{{"r", {{"d", "old"}}}}})}
	};
	const auto original = FileSource("Node_ORA_File_Read", Json::array({consumer}));
	const PxcxStructureEdit deletion = PxcxNodeDelete{"removed", false};
	Diagnostic diagnostic;
	std::vector<std::byte> removedBytes;
	REQUIRE(WritePxcxStructureEdits(
		original, original.Source.OriginalBytes, std::span(&deletion, 1), removedBytes, diagnostic
	));
	engine::bake::PxcxArchive checked;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(removedBytes, checked, failure));
	PxcxImport imported;
	REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	REQUIRE(imported.Graph.Links.empty());
	const auto capture = Layers(imported.Graph.Nodes[0]);
	const auto replay = Bound(imported.Graph);
	Document candidate;
	GroupReplayState next;
	REQUIRE(
		ApplySourceArtworkEdit(
			imported.Graph,
			capture,
			replay,
			{SourceArtworkAction::GenerateLayers, true, true, 7, 8, std::nullopt},
			candidate,
			next,
			diagnostic
		) == Status::Ok
	);
	REQUIRE(candidate.Nodes.size() == 3);
	CHECK(std::none_of(candidate.Nodes.begin(), candidate.Nodes.end(), [](const auto &node) {
		return node.Id == "removed";
	}));
	CHECK(candidate.Nodes[1].SourceDisplayName == "Ink layer");
	CHECK(candidate.Nodes[1].SourceInternalName == "Ink_layer");
	std::vector<std::byte> saved;
	REQUIRE(WritePxcxProjection(imported, candidate, {3, .5, true}, saved, diagnostic));
	REQUIRE(engine::bake::ReadPxcx(saved, checked, failure));
	const auto graph = Json::parse(checked.GraphJson.c_str());
	CHECK(graph["future_project"] == 41);
	CHECK(graph["nodes"][0]["future_file"] == "retained");
	PxcxImport reloaded;
	REQUIRE(ImportPxcxImageGraph(checked, reloaded, failure));
	std::vector<std::byte> noOp;
	REQUIRE(WritePxcxProjection(reloaded, reloaded.Graph, {}, noOp, diagnostic));
	CHECK(noOp == saved);
}

TEST_CASE(
	"Empty ASE ORA and Krita layer-name lists retain constructor defaults without physical keys",
	"[imagegraphio][artwork]"
) {
	for (const std::string family : {"ASE", "ORA", "Krita"}) {
		CAPTURE(family);
		Json inputs = Json::array({Json{{"from_node", "file"}, {"from_index", 1}}});
		if (family == "ASE") inputs.push_back(Json{{"r", {{"d", false}}}});
		inputs.push_back(Json{{"anim", true}, {"r", Json::array()}, {"future_control", 173}});
		if (family == "ASE") {
			inputs.push_back(Json{{"r", {{"d", false}}}});
			inputs.push_back(Json{{"r", {{"d", true}}}});
		}
		const auto imported = FileSource(
			"Node_" + family + "_File_Read",
			Json::array({Json{
				{"id", "layer"},
				{"type", "Node_" + family + "_layer"},
				{"name", "Layer"},
				{"iname", "retained_layer"},
				{"x", 20},
				{"y", 180},
				{"inputs", std::move(inputs)},
				{"future_layer", 179}
			}})
		);
		const auto &layer = imported.Graph.Nodes[1];
		CHECK(
			layer.Type == (family == "ASE"	 ? "pc.ase_layer"
						   : family == "ORA" ? "pc.ora_layer"
											 : "pc.krita_layer")
		);
		const auto value = std::find_if(layer.Values.begin(), layer.Values.end(), [](const auto &control) {
			return control.Port == "layer_name";
		});
		REQUIRE(value != layer.Values.end());
		CHECK(value->Data == Value{std::string{}});
		CHECK(
			std::find(layer.SourceAnimatedInputs.begin(), layer.SourceAnimatedInputs.end(), "layer_name") !=
			layer.SourceAnimatedInputs.end()
		);
		CHECK(
			std::none_of(
				imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
					return key.NodeId == "layer" && key.Port == "layer_name";
				}
			)
		);
		CHECK(std::any_of(imported.Graph.Tracks.begin(), imported.Graph.Tracks.end(), [](const auto &track) {
			return track.NodeId == "layer" && track.Port == "layer_name";
		}));
		Diagnostic diagnostic;
		std::vector<std::byte> bytes;
		const bool saved = WritePxcxProjection(imported, imported.Graph, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(saved);
		CHECK(bytes == imported.Source.OriginalBytes);
	}
}
