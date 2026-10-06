#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_mirror_path_ratio")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::json;
	PxcxImport
	Imported(bool animated = false, bool empty = false, bool separated = false, bool emptyAxes = false) {
		Json inputs = Json::array();
		for (size_t i = 0; i < 16; ++i)
			inputs.push_back(Json::object());
		inputs[0] = {{"from_node", "noise"}, {"from_index", 0}};
		inputs[1] = {
			{"from_node", "number"},
			{"from_index", 0},
			{"unit", 1},
			{"anim", animated},
			{"future", "keep-local"}
		};
		if (animated) {
			inputs[1]["r"] = Json::array(
				{Json::array(
					 {Json::array({0, 0}),
					  Json::array({.25, .1}),
					  Json::array({0, 1}),
					  Json::array({0, 0}),
					  0,
					  0,
					  true,
					  0,
					  16777215}
				 ),
				 Json::array(
					 {Json::array({0, 2}),
					  Json::array({.75, .9}),
					  Json::array({0, 1}),
					  Json::array({0, 0}),
					  0,
					  0,
					  true,
					  0,
					  16777215}
				 )}
			);
		} else
			inputs[1]["r"] = {{"d", Json::array({.25, .9})}};
		if (empty) inputs[1]["r"] = Json::array();
		if (separated) {
			inputs[1]["sep_axis"] = true;
			const auto key = [](double tick, double value, const char *tail) {
				return Json::array(
					{Json::array({0, tick}),
					 value,
					 Json::array({0, 1}),
					 Json::array({0, 0}),
					 0,
					 0,
					 true,
					 0,
					 16777215,
					 Json{{"future", tail}}}
				);
			};
			inputs[1]["animators"] = Json::array(
				{Json::array({key(0, .25, "x0"), key(10, .75, "x1")}),
				 Json{{"d", .9}},
				 Json{{"future_axis", "keep-third"}}}
			);
			if (emptyAxes) {
				inputs[1]["animators"][0] = Json::array();
				inputs[1]["animators"][1] = Json::array();
			}
		}

		Json root = {
			{"animator", {{"frames_total", 30}, {"playback", 1}, {"framerate", 30}}},
			{"nodes",
			 Json::array(
				 {{{"id", "number"},
				   {"type", "Node_Number_Simple"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs", Json::array({Json{{"r", {{"d", .5}}}}})}},
				  {{"id", "noise"},
				   {"type", "Node_Noise_Simplex"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs", Json::array()}},
				  {{"id", "mirror"}, {"type", "Node_Mirror_Polar"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		};
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = root.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		const auto node =
			std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &n) {
				return n.Id == "mirror";
			});
		REQUIRE(node != imported.Graph.Nodes.end());
		INFO(failure);
		REQUIRE(node->Type == "pc.mirror_polar");
		return imported;
	}
	PxcxImport ReadEdited(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
	const Node &Mirror(const PxcxImport &project) {
		const auto node =
			std::find_if(project.Graph.Nodes.begin(), project.Graph.Nodes.end(), [](const auto &n) {
				return n.Id == "mirror";
			});
		REQUIRE(node != project.Graph.Nodes.end());
		return *node;
	}
	Json Center(const PxcxImport &project) {
		const auto text =
			std::string_view(project.Source.GraphJson.data(), project.Source.GraphJson.size() - 1);
		const Json root = Json::parse(text);
		for (const auto &node : root["nodes"])
			if (node["id"] == "mirror") return node["inputs"][1];
		FAIL("source Mirror is missing");
		return {};
	}
}
TEST_CASE("PXC linked Mirror preserves editable local ratio and source connection", "[pxcx_mirror_ratio]") {
	const auto imported = Imported();
	const auto &node = Mirror(imported);
	const auto raw = std::find_if(node.Values.begin(), node.Values.end(), [](const auto &v) {
		return v.Port == "center";
	});
	REQUIRE(raw != node.Values.end());
	CHECK(raw->Data == Value{Vector2{.25, .9}});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(imported.Graph), restored, diag) == Status::Ok);
	CHECK(restored == imported.Graph);
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"mirror", "center", Vector2{.75, .1}}};
	std::vector<std::byte> bytes;
	const auto ok = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diag);
	INFO(diag.Message);
	REQUIRE(ok);
	const auto replay = ReadEdited(bytes);
	const auto center = Center(replay);
	CHECK(center["from_node"] == "number");
	CHECK(center["from_index"] == 0);
	CHECK(center["unit"] == 1);
	CHECK(center["future"] == "keep-local");
	CHECK(center["r"]["d"] == Json::array({.75, .1}));
	CHECK(replay.Graph.Links == imported.Graph.Links);
}
TEST_CASE("PXC linked Mirror animator key inverse retains ordered raw tuples", "[pxcx_mirror_ratio]") {
	const auto imported = Imported(true);
	const auto first =
		std::find_if(imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "mirror" && key.Port == "center";
		});
	REQUIRE(first != imported.Graph.Keyframes.end());
	Keyframe replacement = *first;
	replacement.Data = Vector2{.125, .8};
	const std::array<PxcxEdit, 1> edits{
		PxcxKeyframeEdit{"mirror", "center", GetFrameTime(*first), replacement}
	};
	Diagnostic diag;
	std::vector<std::byte> bytes;
	const auto ok = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diag);
	INFO(diag.Message);
	REQUIRE(ok);
	const auto replay = ReadEdited(bytes);
	const auto center = Center(replay);
	CHECK(center["from_node"] == "number");
	CHECK(center["anim"] == true);
	CHECK(center["future"] == "keep-local");
	CHECK(center["r"][0][1] == Json::array({.125, .8}));
	CHECK(center["r"][1][1] == Json::array({.75, .9}));
	CHECK(replay.Graph.Links == imported.Graph.Links);
}

TEST_CASE("PXC empty saved Mirror animator reloads the source constructor ratio", "[pxcx_mirror_ratio]") {
	const auto imported = Imported(true, true);
	const auto &node = Mirror(imported);
	CHECK(std::none_of(node.Values.begin(), node.Values.end(), [](const auto &value) {
		return value.Port == "center";
	}));
	CHECK(std::none_of(imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "mirror" && key.Port == "center";
	}));
	CHECK(
		std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), "center") !=
		node.SourceAnimatedInputs.end()
	);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	CHECK(Center(imported)["r"] == Json::array());
}
TEST_CASE(
	"PXC full projection rewrites linked Mirror animator without disconnecting", "[pxcx_mirror_ratio]"
) {
	const auto imported = Imported(true);
	auto desired = imported.Graph;
	const auto key = std::find_if(desired.Keyframes.begin(), desired.Keyframes.end(), [](const auto &k) {
		return k.NodeId == "mirror" && k.Port == "center";
	});
	REQUIRE(key != desired.Keyframes.end());
	key->Data = Vector2{.125, .875};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message << " " << diagnostic.NodeId << ":" << diagnostic.Port);
	REQUIRE(saved);
	const auto replay = ReadEdited(bytes);
	CHECK(replay.Graph == desired);
	const auto center = Center(replay);
	CHECK(center["from_node"] == "number");
	CHECK(center["unit"] == 1);
	CHECK(center["future"] == "keep-local");
	CHECK(center["r"][0][1] == Json::array({.125, .875}));
}
TEST_CASE("PXC split Mirror preserves two physical axes and dormant linked animator", "[pxcx_mirror_axes]") {
	const auto imported = Imported(true, false, true);
	const auto &node = Mirror(imported);
	REQUIRE(node.SourceSeparatedVec2Animators);
	REQUIRE(node.SourceSeparatedVec2Animators->Inputs.size() == 1);
	const auto &axes = node.SourceSeparatedVec2Animators->Inputs[0].Axes;
	REQUIRE(axes[0].Keys.size() == 2);
	CHECK(axes[0].Keys[0].Data == Value{.25});
	CHECK(axes[0].Keys[1].Data == Value{.75});
	CHECK(axes[0].Keys[0].SourceKeyId.starts_with("animators/0/"));
	REQUIRE(axes[1].Keys.size() == 1);
	CHECK(axes[1].Keys[0].Data == Value{.9});
	CHECK(axes[1].Keys[0].SourceKeyId == "animators/1/pxc:compact");
	CHECK(imported.Graph.Keyframes.size() == 3);
	std::vector<Value> dormant;
	for (const auto &key : imported.Graph.Keyframes)
		if (key.NodeId == "mirror" && key.Port == "center") dormant.push_back(key.Data);
	REQUIRE(dormant.size() == 2);
	CHECK(dormant[0] == Value{Vector2{.25, .1}});
	CHECK(dormant[1] == Value{Vector2{.75, .9}});
	CHECK(
		std::count_if(imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "number" && key.Data == Value{.5};
		}) == 1
	);
	CHECK(Center(imported)["animators"][2]["future_axis"] == "keep-third");
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
}
TEST_CASE(
	"PXC split axis inverse edits X while retaining Y links units opaque key fields and extra axes",
	"[pxcx_mirror_axes]"
) {
	const auto imported = Imported(true, false, true);
	auto desired = imported.Graph;
	auto node = std::find_if(desired.Nodes.begin(), desired.Nodes.end(), [](const auto &n) {
		return n.Id == "mirror";
	});
	REQUIRE(node != desired.Nodes.end());
	node->SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0].Data = .125;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message << " " << diagnostic.NodeId << ":" << diagnostic.Port);
	REQUIRE(saved);
	const auto replay = ReadEdited(bytes);
	CHECK(replay.Graph == desired);
	const auto before = Center(imported), after = Center(replay);
	CHECK(after["animators"][0][0][1] == .125);
	CHECK(after["animators"][0][0][9] == before["animators"][0][0][9]);
	CHECK(after["animators"][0][1] == before["animators"][0][1]);
	CHECK(after["animators"][1] == before["animators"][1]);
	CHECK(after["animators"][2] == before["animators"][2]);
	CHECK(after["r"] == before["r"]);
	CHECK(after["from_node"] == "number");
	CHECK(after["unit"] == 1);
	CHECK(after["future"] == "keep-local");
	CHECK(replay.Graph.Links == imported.Graph.Links);
}
TEST_CASE(
	"PXC saved empty split axes restore constructor scalars while empty live axes cannot silently reload",
	"[pxcx_mirror_axes]"
) {
	const auto imported = Imported(true, false, true, true);
	const auto &axes = Mirror(imported).SourceSeparatedVec2Animators->Inputs[0].Axes;
	REQUIRE(axes[0].Keys.size() == 1);
	REQUIRE(axes[1].Keys.size() == 1);
	CHECK(axes[0].Keys[0].Data == Value{.5});
	CHECK(axes[1].Keys[0].Data == Value{.5});
	CHECK(axes[0].Keys[0].SourceKeyId.empty());
	auto desired = imported.Graph;
	auto node = std::find_if(desired.Nodes.begin(), desired.Nodes.end(), [](const auto &n) {
		return n.Id == "mirror";
	});
	node->SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys.clear();
	Diagnostic diagnostic;
	const std::vector<std::byte> prior{std::byte{42}};
	auto bytes = prior;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(bytes == prior);
	CHECK(diagnostic.Message.find("empty live scalar axis") != std::string::npos);
}
TEST_CASE(
	"PXC disabling split axes preserves their opaque archive payload and restores dormant Vec2",
	"[pxcx_mirror_axes]"
) {
	const auto imported = Imported(true, false, true);
	auto desired = imported.Graph;
	auto node = std::find_if(desired.Nodes.begin(), desired.Nodes.end(), [](const auto &n) {
		return n.Id == "mirror";
	});
	node->SourceSeparatedVec2Animators->Inputs[0].Separated = false;
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	const auto replay = ReadEdited(bytes);
	CHECK(replay.Graph == desired);
	CHECK(Center(replay)["sep_axis"] == false);
	CHECK(Center(replay)["animators"] == Center(imported)["animators"]);
	CHECK(Center(replay)["r"] == Center(imported)["r"]);
}
TEST_CASE(
	"PXC split storage roundtrips retained duplicate-time axis keys without rebasing their order",
	"[pxcx_mirror_axes]"
) {
	const auto imported = Imported(true, false, true);
	auto desired = imported.Graph;
	auto node = std::find_if(desired.Nodes.begin(), desired.Nodes.end(), [](const auto &n) {
		return n.Id == "mirror";
	});
	auto &keys = node->SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys;
	keys[0].Tick = 5;
	keys.push_back({"mirror", "center", 5, .5, "source", KeyframeEase{}});
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	const auto replay = ReadEdited(bytes);
	const auto &actual = Mirror(replay).SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys;
	REQUIRE(actual.size() == 3);
	CHECK(actual[0].Tick == 5);
	CHECK(actual[1].Tick == 10);
	CHECK(actual[2].Tick == 5);
	CHECK(actual[0].Data == Value{.25});
	CHECK(actual[2].Data == Value{.5});
	CHECK(actual[0].SourceKeyId != actual[2].SourceKeyId);
	CHECK(Center(replay)["animators"][0][0][9] == Center(imported)["animators"][0][0][9]);
}

TEST_CASE("PXC malformed axes stay opaque and over-limit axes preserve prior import", "[pxcx_mirror_axes]") {
	const auto imported = Imported(true, false, true);
	const auto root =
		Json::parse(std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1));
	for (const bool oversized : {false, true}) {
		auto changed = root;
		auto &axis = changed["nodes"][2]["inputs"][1]["animators"][0];
		if (oversized) {
			const auto key = axis[0];
			axis = Json::array();
			for (size_t i = 0; i < Limits::MaximumKeyframes; ++i) {
				auto frame = key;
				frame[0][1] = i;
				axis.push_back(std::move(frame));
			}
		} else
			axis[0][1] = Json::array({.1, .2});
		auto archive = imported.Source;
		archive.GraphJson = changed.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		auto prior = imported;
		if (oversized) {
			CHECK_FALSE(ImportPxcxImageGraph(checked, prior, failure));
			CHECK_FALSE(failure.empty());
			CHECK(prior.Graph == imported.Graph);
			CHECK(prior.Source.OriginalBytes == imported.Source.OriginalBytes);
		} else {
			REQUIRE(ImportPxcxImageGraph(checked, prior, failure));
			const auto node =
				std::find_if(prior.Graph.Nodes.begin(), prior.Graph.Nodes.end(), [](const auto &n) {
					return n.Id == "mirror";
				});
			REQUIRE(node != prior.Graph.Nodes.end());
			CHECK(node->Type != "pc.mirror_polar");
			CHECK_FALSE(node->SourceSeparatedVec2Animators);
			CHECK(NodeClonePayloadBytes(*node).has_value());
			CHECK(std::any_of(prior.Diagnostics.begin(), prior.Diagnostics.end(), [](const auto &d) {
				return d.NodeId == "mirror" && d.Message.find("separated scalar keys") != std::string::npos;
			}));
			CHECK(prior.Source.GraphJson == checked.GraphJson);
		}
	}
}

TEST_CASE("PXC split key admission includes other nodes and dormant ordinary keys", "[pxcx_mirror_axes]") {
	const auto imported = Imported(true, false, true);
	auto root =
		Json::parse(std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1));
	auto &axis = root["nodes"][2]["inputs"][1]["animators"][0];
	const auto original = axis[0];
	axis = Json::array();
	for (size_t i = 0; i < Limits::MaximumKeyframes / 2; ++i) {
		auto key = original;
		key[0][1] = i;
		axis.push_back(std::move(key));
	}
	auto second = root["nodes"][2];
	second["id"] = "mirror-second";
	root["nodes"].push_back(std::move(second));
	auto archive = imported.Source;
	archive.GraphJson = root.dump() + '\0';
	archive.Nodes.clear();
	archive.Links.clear();
	std::vector<std::byte> bytes;
	std::string failure;
	INFO(failure);
	REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
	engine::bake::PxcxArchive checked;
	REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
	auto prior = imported;
	CHECK_FALSE(ImportPxcxImageGraph(checked, prior, failure));
	CHECK(failure.find("aggregate key bounds") != std::string::npos);
	CHECK(prior.Graph == imported.Graph);
	CHECK(prior.Source.OriginalBytes == imported.Source.OriginalBytes);
}

TEST_CASE(
	"PXC combined animator edits retain inactive scalar tracks and opaque fields", "[pxcx_mirror_axes]"
) {
	const auto imported = Imported(true, false, true);
	auto desired = imported.Graph;
	auto node = std::find_if(desired.Nodes.begin(), desired.Nodes.end(), [](const auto &n) {
		return n.Id == "mirror";
	});
	REQUIRE(node != desired.Nodes.end());
	node->SourceSeparatedVec2Animators->Inputs[0].Separated = false;
	const auto key = std::find_if(desired.Keyframes.begin(), desired.Keyframes.end(), [](const auto &k) {
		return k.NodeId == "mirror" && k.Port == "center";
	});
	REQUIRE(key != desired.Keyframes.end());
	key->Data = Vector2{.4, .8};
	Diagnostic diagnostic;
	std::vector<std::byte> bytes;
	INFO(diagnostic.Message);
	REQUIRE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	const auto restored = ReadEdited(bytes);
	CHECK(restored.Graph == desired);
	CHECK(Center(restored)["sep_axis"] == false);
	CHECK(Center(restored)["animators"] == Center(imported)["animators"]);
	CHECK(Center(restored)["future"] == "keep-local");
	CHECK(Center(restored)["r"] != Center(imported)["r"]);
}
