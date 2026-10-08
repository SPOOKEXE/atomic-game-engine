#include "ImageGraphCanvasInputs.hpp"
#include "ImageGraphDocumentEdit.hpp"
#include "ImageGraphGroupHost.hpp"
#include "ImageGraphHost.hpp"
#include "TimelineDopesheet.hpp"

#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <nodegraph/Editor.hpp>
#include <studio/PxcxSave.hpp>
#include <tuple>
TEST_SUITE_ID("studio.composer_workflow")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("studio.pxcxsave")
namespace {
	using namespace engine::imagegraph;
	engine::imagegraphio::PxcxImport Source(bool opaque = false) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = R"PXC({"animator":{"frames_total":20,"playback":0,"framerate":24},"nodes":[
 {"id":"group","type":"Node_Group","x":0,"y":0,"inputs":[{"r":{"d":[2,8]}},{"r":{"d":["kept",4,true]}},{"r":{"d":false}}],"attri":{"custom_input_list":["range","any","trigger"],"custom_output_list":["range-out","any-out","trigger-out"],"color_depth":1,"interpolate":0,"oversample":0}},
 {"id":"range","type":"Node_Group_Input","group":"group","x":0,"y":0,"inputs":[{"r":{"d":1}},{"r":{"d":[0,10]}},{"r":{"d":1}},{},{},{},{},{},{},{},{},{},{},{},{},{}]},
 {"id":"any","type":"Node_Group_Input","group":"group","x":0,"y":0,"inputs":[{"r":{"d":0}},{},{"r":{"d":11}},{},{},{},{},{},{},{},{},{},{},{},{},{}]},
 {"id":"trigger","type":"Node_Group_Input","group":"group","x":0,"y":0,"inputs":[{"r":{"d":0}},{},{"r":{"d":19}},{},{},{},{},{},{},{},{},{},{},{},{},{}]},
 {"id":"range-out","type":"Node_Group_Output","group":"group","x":0,"y":0,"inputs":[{"from_node":"range","from_index":0,"from_tag":0}]},
 {"id":"any-out","type":"Node_Group_Output","group":"group","x":0,"y":0,"inputs":[{"from_node":"any","from_index":0,"from_tag":0}]},
 {"id":"trigger-out","type":"Node_Group_Output","group":"group","x":0,"y":0,"inputs":[{"from_node":"trigger","from_index":0,"from_tag":0}]},
 {"id":"range-sink","type":"Node_Project_Output","x":0,"y":0,"inputs":[{"from_node":"group","from_index":0,"from_tag":0}]},
 {"id":"any-sink","type":"Node_Project_Output","x":0,"y":0,"inputs":[{"from_node":"group","from_index":1,"from_tag":0}]},
 {"id":"trigger-sink","type":"Node_Project_Output","x":0,"y":0,"inputs":[{"from_node":"group","from_index":2,"from_tag":0}]},
 {"id":"lua","type":"Node_Lua_Compute","x":0,"y":0,"inputs":[{"r":{"d":"unusedFailure"}},{"r":{"d":0}},{"r":{"d":"error('unselected failure')"}},{},{"r":{"d":true}}]},
 {"id":"lua-sink","type":"Node_Project_Output","x":0,"y":0,"inputs":[{"from_node":"lua","from_index":1,"from_tag":0}]}],"future":{"keep":17}
})PXC";
		if (opaque) {
			const std::string compact = "{\"r\":{\"d\":false}}";
			const auto at = archive.GraphJson.find(compact);
			REQUIRE(at != std::string::npos);
			archive.GraphJson.replace(
				at,
				compact.size(),
				R"PXC({"anim":true,"r":[[[0,5],true,[0,1],[0,0],0,0,true,0,16777215,{"opaque_key":{"keep":43}}]]})PXC"
			);
		}
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		engine::imagegraphio::PxcxImport result;
		const auto imported = engine::imagegraphio::ImportPxcxImageGraph(archive, result, failure);
		INFO(failure);
		REQUIRE(imported);
		return result;
	}
	struct CountingHost : HostNodeProvider {
		studio::detail::ImageGraphHost Host;
		unsigned Calls = 0;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &capture, std::string &failure
		) override {
			++Calls;
			return Host.Capture(invocation, capture, failure);
		}
	};
	struct Workflow {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		engine::imagegraphio::PxcxImport Imported = Source();
		Document Doc = Imported.Graph;
		studio::ImageGraphHistory History;
		studio::ImageGraphGroupHost Groups;
		studio::TimelineKeyEditor Keys;
		studio::TimelineDopesheet Sheet;
		Plan Compiled;
		Diagnostic Error;
		uint64_t Revision = 1;
		unsigned Gestures = 0;
		ImVec2 Min, Max;
		std::unique_ptr<ComposerLuaHost> Lua = engine::script::MakeComposerLuaHost();
		CountingHost Host;
		explicit Workflow(bool opaque = false) : Imported(Source(opaque)), Doc(Imported.Graph) {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Host.Host.Lua = Lua.get();
			REQUIRE(Migrate(Doc, Error) == Status::Ok);
			Compile();
		}
		~Workflow() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		void Compile() {
			auto status = engine::imagegraph::Compile(Doc, Compiled, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
		}
		EvaluationRequest Request(uint64_t tick = 0) {
			EvaluationRequest request;
			request.Tick = tick;
			request.HostProvider = &Host;
			const auto prepared = Groups.Prepare(Doc, Compiled, Revision, request, Error);
			INFO(Error.Message);
			REQUIRE(prepared);
			return request;
		}
		Value Preview(std::string_view output, uint64_t tick = 0) {
			auto request = Request(tick);
			EvaluatedValue value;
			auto status = EvaluateValue(Doc, Compiled, std::string(output), request, value, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			return value.Data;
		}
		void Edit(std::string id, Value value, bool animated = false, uint64_t tick = 0) {
			auto request = Request(tick);
			GroupRefreshEvent event;
			event.NodeId = std::move(id);
			event.EditedPort = "parent_value";
			event.Reason = GroupRefreshReason::ParentEdit;
			event.LocalValue = &value;
			event.LocalAnimated = animated;
			event.At = request;
			auto accepted = Groups.Edit(Doc, History, Revision, event, Error);
			INFO(Error.Message);
			REQUIRE(accepted);
			++Revision;
			Compile();
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Workflow", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			Sheet.Draw(Doc, Revision, Keys, {}, Error, [&] {
				bool accepted = false;
				if (studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &doc) {
						accepted = Sheet.Commit(doc, Keys, Error);
					})) {
					++Revision;
					++Gestures;
					Compile();
				}
				return accepted;
			});
			Min = ImGui::GetItemRectMin();
			Max = ImGui::GetItemRectMax();
			ImGui::End();
			ImGui::Render();
		}
		void Mouse(ImVec2 point) {
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			Frame();
		}
		void DragTrigger() {
			Frame();
			Frame();
			const studio::ImageGraphKeyframeIdentity target{"trigger", "parent_value", {5, 0, false}};
			auto find = [&] {
				return std::find_if(Sheet.Markers.begin(), Sheet.Markers.end(), [&](const auto &marker) {
					return Sheet.MatchesMarker(marker, target);
				});
			};
			auto marker = find();
			REQUIRE(marker != Sheet.Markers.end());
			for (unsigned n = 0; marker->Position.y > Max.y - 10 && n < 40; ++n) {
				Mouse({Min.x + 30, Min.y + 60});
				ImGui::GetIO().AddMouseWheelEvent(0, -1);
				Frame();
				marker = find();
			}
			REQUIRE(marker->Position.y > Min.y + 24);
			REQUIRE(marker->Position.y < Max.y - 10);
			auto point = marker->Position;
			Mouse(point);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			Frame();
			REQUIRE(Sheet.Dragging);
			Mouse({point.x + 2 * float(Sheet.PixelsPerFrame), point.y});
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			Frame();
			Frame();
		}
	};
}
TEST_CASE(
	"combined source Group Range Any Trigger authors undo seeks drags and saves PXC atomically",
	"[studio][composer_workflow]"
) {
	Workflow ui;
	const auto original = ui.Doc;
	const Value range = ArrayValue{ValueType::Scalar, {2., 8.}};
	const auto any = ui.Preview("any-sink");
	REQUIRE(std::get<ArrayValue>(any).Items.size() == 3);
	CHECK(ui.Preview("range-sink") == range);
	CHECK(ui.Preview("trigger-sink") == Value{false});
	CHECK(ui.Host.Calls == 0);
	ui.Edit("range", ArrayValue{ValueType::Scalar, {3., 9.}});
	const auto rangeEdited = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == original);
	CHECK(ui.Preview("range-sink") == range);
	REQUIRE(ui.History.Redo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == rangeEdited);
	auto nextAny = std::get<ArrayValue>(any);
	nextAny.Items[0] = SourceArrayItem{ElementValue{std::string{"edited"}}};
	ui.Edit("any", nextAny);
	ui.Edit("trigger", true, true, 5);
	const auto beforeDrag = ui.Doc;
	CHECK(ui.Preview("trigger-sink", 4) == Value{false});
	CHECK(ui.Preview("trigger-sink", 5) == Value{true});
	CHECK(ui.Preview("trigger-sink", 6) == Value{false});
	ui.DragTrigger();
	INFO(ui.Error.Message);
	REQUIRE(ui.Gestures == 1);
	const auto afterDrag = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == beforeDrag);
	REQUIRE(ui.History.Redo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == afterDrag);
	CHECK(ui.Preview("trigger-sink", 5) == Value{false});
	CHECK(ui.Preview("trigger-sink", 7) == Value{true});
	CHECK(ui.Preview("trigger-sink", 8) == Value{false});
	CHECK(ui.Preview("any-sink") == Value{nextAny});
	CHECK(ui.Host.Calls == 0);
	auto request = ui.Request(7);
	EvaluatedValue retained;
	retained.Data = nextAny;
	CHECK(EvaluateValue(ui.Doc, ui.Compiled, "lua-sink", request, retained, ui.Error) != Status::Ok);
	CHECK(ui.Error.NodeId == "lua");
	CHECK(ui.Host.Calls == 1);
	CHECK(retained.Data == Value{nextAny});
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	Document canvasDoc;
	REQUIRE(studio::LoadImageGraphCanvas(ui.Doc, canvas, ids, failure));
	REQUIRE(studio::SaveImageGraphCanvas(canvas, ui.Doc, ids, canvasDoc, failure));
	CHECK(canvasDoc == ui.Doc);
	auto path = std::filesystem::temp_directory_path() /
				("atomic-composer-workflow-" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pxc");
	const auto saved = studio::SavePxcxProjection(path, ui.Imported.Source, ui.Doc, {}, ui.Error);
	INFO(ui.Error.Message);
	REQUIRE(saved);
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	REQUIRE(file.is_open());
	std::vector<std::byte> bytes(size_t(file.tellg()));
	file.seekg(0);
	file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
	file.close();
	std::filesystem::remove(path);
	engine::bake::PxcxArchive archive;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	CHECK(archive.GraphJson.find("\"keep\":17") != std::string::npos);
	engine::imagegraphio::PxcxImport reloaded;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, reloaded, failure));
	REQUIRE(Migrate(reloaded.Graph, ui.Error) == Status::Ok);
	auto semanticAuthored = ui.Doc;
	for (auto &key : semanticAuthored.Keyframes)
		key.SourceKeyId.clear();
	for (auto &key : reloaded.Graph.Keyframes)
		key.SourceKeyId.clear();
	// PXC import keeps source socket order; dragging keys sorts their native addresses.
	const auto canonicalizeKeys = [](Document &document) {
		std::sort(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &a, const auto &b) {
			return std::tie(a.NodeId, a.Port, a.NegativeFrame, a.Tick, a.Subframe) <
				   std::tie(b.NodeId, b.Port, b.NegativeFrame, b.Tick, b.Subframe);
		});
	};
	canonicalizeKeys(semanticAuthored);
	canonicalizeKeys(reloaded.Graph);
	CHECK(reloaded.Graph == semanticAuthored);
}

TEST_CASE(
	"existing opaque source Group Trigger key survives real drag native save reload and undo",
	"[studio][composer_workflow]"
) {
	Workflow ui(true);
	const auto original = ui.Doc;
	CHECK(ui.Preview("trigger-sink", 5) == Value{true});
	ui.DragTrigger();
	REQUIRE(ui.Gestures == 1);
	const auto edited = ui.Doc;
	CHECK(ui.Preview("trigger-sink", 5) == Value{false});
	CHECK(ui.Preview("trigger-sink", 7) == Value{true});
	auto path = std::filesystem::temp_directory_path() /
				("atomic-composer-opaque-" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pxc");
	{
		std::ofstream file(path, std::ios::binary);
		REQUIRE(file.is_open());
		file.write(
			reinterpret_cast<const char *>(ui.Imported.Source.OriginalBytes.data()),
			ui.Imported.Source.OriginalBytes.size()
		);
		REQUIRE(file.good());
	}
	const auto saved = studio::SavePxcxProjection(path, ui.Imported.Source, ui.Doc, {}, ui.Error);
	INFO(ui.Error.Message);
	REQUIRE(saved);
	CHECK(ui.Doc == edited);
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	REQUIRE(file.is_open());
	std::vector<std::byte> bytes(size_t(file.tellg()));
	file.seekg(0);
	file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
	file.close();
	std::filesystem::remove(path);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	CHECK(archive.GraphJson.find("\"opaque_key\":{\"keep\":43}") != std::string::npos);
	engine::imagegraphio::PxcxImport reloaded;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, reloaded, failure));
	auto originalKey = std::find_if(edited.Keyframes.begin(), edited.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "trigger" && key.Port == "parent_value";
	});
	auto savedKey =
		std::find_if(reloaded.Graph.Keyframes.begin(), reloaded.Graph.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "trigger" && key.Port == "parent_value";
		});
	REQUIRE(originalKey != edited.Keyframes.end());
	REQUIRE(savedKey != reloaded.Graph.Keyframes.end());
	CHECK(savedKey->Tick == 7);
	CHECK_FALSE(originalKey->SourceKeyId.empty());
	CHECK_FALSE(savedKey->SourceKeyId.empty());
	CHECK(savedKey->SourceKeyId != originalKey->SourceKeyId);
	Document nativeReload;
	REQUIRE(Read(Write(edited), nativeReload, ui.Error) == Status::Ok);
	CHECK(nativeReload == edited);
	REQUIRE(ui.History.Undo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == original);
	CHECK(ui.Preview("trigger-sink", 5) == Value{true});
	REQUIRE(ui.History.Redo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == edited);
	CHECK(ui.Preview("trigger-sink", 7) == Value{true});
}

TEST_CASE(
	"source key clipboard creates fresh provenance and explicit deletion keeps only the new key",
	"[studio][composer_workflow]"
) {
	Workflow ui(true);
	const auto original = ui.Doc;
	const std::array<studio::ImageGraphKeyframeIdentity, 1> selected{
		{{"trigger", "parent_value", {5, 0, false}}}
	};
	std::vector<Keyframe> clipboard;
	REQUIRE(studio::CaptureImageGraphKeyframes(ui.Doc, selected, clipboard, ui.Error));
	REQUIRE(clipboard.size() == 1);
	CHECK_FALSE(clipboard[0].SourceKeyId.empty());
	bool copied = false;
	REQUIRE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](Document &document) {
		copied = studio::TransferImageGraphKeyframes(
			document, clipboard, {5, 0, false}, {9, 0, false}, true, ui.Error
		);
	}));
	REQUIRE(copied);
	++ui.Revision;
	ui.Compile();
	auto sourceKey = std::find_if(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "trigger" && key.Port == "parent_value" && key.Tick == 5;
	});
	auto clone = std::find_if(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "trigger" && key.Port == "parent_value" && key.Tick == 9;
	});
	REQUIRE(sourceKey != ui.Doc.Keyframes.end());
	REQUIRE(clone != ui.Doc.Keyframes.end());
	CHECK(sourceKey->SourceKeyId == clipboard[0].SourceKeyId);
	CHECK(clone->SourceKeyId.empty());
	const auto copiedDocument = ui.Doc;
	std::vector<std::byte> bytes;
	REQUIRE(engine::imagegraphio::WritePxcxProjection(ui.Imported, ui.Doc, {}, bytes, ui.Error));
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	const auto opaque = archive.GraphJson.find("\"opaque_key\"");
	REQUIRE(opaque != std::string::npos);
	CHECK(archive.GraphJson.find("\"opaque_key\"", opaque + 1) == std::string::npos);
	CHECK(ui.Preview("trigger-sink", 5) == Value{true});
	CHECK(ui.Preview("trigger-sink", 9) == Value{true});
	CHECK(ui.Preview("trigger-sink", 6) == Value{false});
	CHECK(ui.Preview("trigger-sink", 8) == Value{false});
	REQUIRE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](Document &document) {
		std::erase_if(document.Keyframes, [](const auto &key) {
			return key.NodeId == "trigger" && key.Port == "parent_value" && key.Tick == 5;
		});
	}));
	++ui.Revision;
	ui.Compile();
	REQUIRE(engine::imagegraphio::WritePxcxProjection(ui.Imported, ui.Doc, {}, bytes, ui.Error));
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	CHECK(archive.GraphJson.find("\"opaque_key\"") == std::string::npos);
	CHECK(ui.Preview("trigger-sink", 5) == Value{false});
	CHECK(ui.Preview("trigger-sink", 9) == Value{true});
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == copiedDocument);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == original);
}

TEST_CASE(
	"source canvas grouped-node placement compiles through the actual "
	"SyncCanvas seed path",
	"[studio][composer_workflow53]"
) {
	Workflow ui;
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	REQUIRE(studio::LoadImageGraphCanvas(ui.Doc, graph, ids, failure));
	for (const char *type : {"pc.struct", "pc.lua_compute", "pc.pb_draw_rectangle"}) {
		INFO(type);
		const auto created = graph.Add(type, 600, 100);
		REQUIRE(created != nodegraph::NO_NODE);
		studio::detail::SeedImageGraphCanvasInputs(graph, ids);
		REQUIRE(graph.Find(created)->DynamicInputs.empty());
		Document next;
		REQUIRE(studio::SaveImageGraphCanvas(graph, ui.Doc, ids, next, failure));
		const auto authored = std::find_if(next.Nodes.begin(), next.Nodes.end(), [&](const auto &node) {
			return node.Type == type;
		});
		REQUIRE(authored != next.Nodes.end());
		const auto id = authored->Id;
		const auto grouped = studio::SetSourceImageGraphDynamicGroupCount(next, id, 1, ui.Error);
		INFO(ui.Error.Message);
		REQUIRE(grouped);
		Plan plan;
		const auto status = Compile(next, plan, ui.Error);
		INFO(ui.Error.Message << " node=" << ui.Error.NodeId << " port=" << ui.Error.Port);
		REQUIRE(status == Status::Ok);
		REQUIRE(graph.Remove(created));
	}
}

TEST_CASE(
	"combined Group Range Any Trigger canvas gestures commit one undo "
	"and preserve source save",
	"[studio][composer_workflow53]"
) {
	Workflow ui;
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	std::string failure;
	REQUIRE(studio::LoadImageGraphCanvas(ui.Doc, graph, ids, failure));
	const auto target = ids.ToCanvas.at("trigger-sink");
	graph.Find(target)->X = 500;
	graph.Find(target)->Y = 200;
	Document positioned;
	REQUIRE(studio::SaveImageGraphCanvas(graph, ui.Doc, ids, positioned, failure));
	ui.Doc = positioned;
	ui.Compile();
	const auto before = ui.Doc;
	nodegraph::Canvas canvas;
	unsigned commits = 0;
	canvas.Signals.Changed = [&] {
		Document edited;
		REQUIRE(studio::SaveImageGraphCanvas(graph, ui.Doc, ids, edited, failure));
		REQUIRE(ui.History.TryRecord(ui.Doc, edited));
		ui.Doc = std::move(edited);
		++ui.Revision;
		++commits;
		ui.Compile();
	};
	ImVec2 origin;
	auto frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({1000, 600});
		ImGui::Begin(
			"canvas52",
			nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
		);
		origin = ImGui::GetCursorScreenPos();
		canvas.Draw(graph);
		ImGui::End();
		ImGui::Render();
	};
	frame();
	frame();
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(origin.x + 530, origin.y + 210);
	frame();
	io.AddMouseButtonEvent(0, true);
	frame();
	io.AddMousePosEvent(origin.x + 590, origin.y + 250);
	frame();
	CHECK(commits == 0);
	// Node drags promote after the canvas's hold interval, then commit on release.
	for (unsigned held = 0; held < 10; ++held)
		frame();
	CHECK(commits == 0);
	io.AddMouseButtonEvent(0, false);
	frame();
	frame();
	REQUIRE(commits == 1);
	const auto moved = ui.Doc;
	const auto sink = std::find_if(ui.Doc.Nodes.begin(), ui.Doc.Nodes.end(), [](const auto &node) {
		return node.Id == "trigger-sink";
	});
	REQUIRE(sink != ui.Doc.Nodes.end());
	CHECK(sink->Position.X == 560);
	CHECK(sink->Position.Y == 240);
	CHECK(ui.Doc.Links == before.Links);
	CHECK(ui.Preview("trigger-sink", 0) == Value{false});
	REQUIRE(ui.History.Undo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	++ui.Revision;
	ui.Compile();
	CHECK(ui.Doc == moved);
	std::vector<std::byte> bytes;
	REQUIRE(engine::imagegraphio::WritePxcxProjection(ui.Imported, ui.Doc, {}, bytes, ui.Error));
	engine::bake::PxcxArchive archive;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	engine::imagegraphio::PxcxImport reloaded;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, reloaded, failure));
	REQUIRE(Migrate(reloaded.Graph, ui.Error) == Status::Ok);
	CHECK(reloaded.Graph == ui.Doc);
}
