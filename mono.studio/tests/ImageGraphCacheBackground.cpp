#include "../src/ImageGraphCacheBackground.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <limits>
#include <nodegraph/Registry.hpp>
#include <nodegraph/Serialize.hpp>
#include <numbers>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.cache_background")

namespace {
	using namespace engine::imagegraph;
	using namespace studio::detail;

	struct Context {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Value = ImGui::CreateContext();

		Context() {
			ImGui::SetCurrentContext(Value);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 700};
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Context() {
			ImGui::SetCurrentContext(Value);
			ImGui::DestroyContext(Value);
			ImGui::SetCurrentContext(Previous);
		}
	};

	void RegisterBackgroundNode(float width = 120.0f) {
		nodegraph::NodeType type;
		type.Id = "fixture.cache-background";
		type.Title = "Cache background";
		type.Width = width;
		nodegraph::NodeTypes::Register(type);
	}

	double Cross(ImVec2 a, ImVec2 b, ImVec2 c) {
		return (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
	}

	bool ContainsNear(const std::vector<ImVec2> &points, float x, float y, float tolerance = 0.001f) {
		return std::any_of(points.begin(), points.end(), [&](ImVec2 point) {
			return std::fabs(point.x - x) <= tolerance && std::fabs(point.y - y) <= tolerance;
		});
	}

	bool SamePoints(const std::vector<ImVec2> &left, const std::vector<ImVec2> &right) {
		if (left.size() != right.size()) return false;
		for (size_t i = 0; i < left.size(); ++i)
			if (left[i].x != right[i].x || left[i].y != right[i].y) return false;
		return true;
	}

	int FirstVertexWithColor(ImU32 color) {
		const auto *data = ImGui::GetDrawData();
		int offset = 0;
		for (int list = 0; list < data->CmdListsCount; ++list) {
			for (const auto &vertex : data->CmdLists[list]->VtxBuffer) {
				if (vertex.col == color) return offset;
				++offset;
			}
		}
		return -1;
	}

	uint64_t ResidentBytes(const ImageGraphCacheBackgrounds &backgrounds) {
		uint64_t bytes = backgrounds.Shapes.capacity() * sizeof(ImageGraphCacheBackground);
		for (const auto &shape : backgrounds.Shapes)
			bytes +=
				shape.Boxes.capacity() * sizeof(ImageGraphCacheBox) + shape.Hull.capacity() * sizeof(ImVec2);
		return bytes;
	}

	struct DrawHarness {
		Context ImGuiContext;
		nodegraph::Graph Graph;
		nodegraph::Canvas Canvas;
		CacheGroupReplayState Groups;
		std::unordered_map<std::string, nodegraph::NodeId> Ids;
		ImageGraphCacheBackgrounds Backgrounds;
		Diagnostic Error;
		bool DrawAccepted = false;
		nodegraph::ViewFrame LastView;
		uint64_t MaximumBytes = 64u * 1024u * 1024u;
		nodegraph::NodeId Owner = nodegraph::NO_NODE, Member = nodegraph::NO_NODE;

		DrawHarness() {
			RegisterBackgroundNode();
			Owner = Graph.Add("fixture.cache-background", 30, 40);
			Member = Graph.Add("fixture.cache-background", -90, 10);
			Graph.Find(Member)->Owner = 900;
			Ids = {{"cache", Owner}, {"producer", Member}};
			Groups.Owners = {{"cache", true, {"cache", "producer", "producer"}}};
			Groups.Nodes = {
				{"cache", "pc.cache", {}, true, {}}, {"producer", "pc.solid", "cache", false, {}}
			};
			Canvas.Select(Member);
			Canvas.Look.NodeBody = 0xFF150B27u;
			Canvas.Signals.DrawBackground = [this](const auto &graph, const auto &view) {
				LastView = view;
				DrawAccepted = DrawImageGraphCacheBackgrounds(
					graph, Canvas, view, Groups, Ids, Backgrounds, Error, MaximumBytes
				);
			};
		}

		void Frame() {
			ImGui::SetCurrentContext(ImGuiContext.Value);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({500, 400});
			ImGui::Begin(
				"Cache background canvas",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
			);
			Canvas.Draw(Graph);
			ImGui::End();
			ImGui::Render();
		}
	};
}

TEST_CASE("cache hull rounds and joins owner and member boxes with a convex boundary", "[studio][cache]") {
	const std::vector<ImageGraphCacheBox> boxes = {{1, 0, 0, 100, 40}, {2, -100, 0, 10, 40}};
	const auto hull = BuildImageGraphCacheHull(boxes);
	REQUIRE(hull.size() >= 3);
	float left = hull.front().x, right = left, top = hull.front().y, bottom = top;
	for (const auto point : hull) {
		left = std::min(left, point.x);
		right = std::max(right, point.x);
		top = std::min(top, point.y);
		bottom = std::max(bottom, point.y);
	}
	CHECK(std::fabs(left - (-132.0f)) < 0.001f);
	CHECK(std::fabs(right - 54.0f) < 0.001f);
	CHECK(std::fabs(top - (-32.0f)) < 0.001f);
	CHECK(std::fabs(bottom - 72.0f) < 0.001f);
	const float arcX = 50.0f + 4.0f * std::cos(float(std::numbers::pi / 12.0));
	const float arcY = -28.0f - 4.0f * std::sin(float(std::numbers::pi / 12.0));
	CHECK(ContainsNear(hull, arcX, arcY));
	for (size_t i = 0; i < hull.size(); ++i)
		CHECK(Cross(hull[i], hull[(i + 1) % hull.size()], hull[(i + 2) % hull.size()]) > 0.0);
	for (size_t i = 0; i < hull.size(); ++i)
		for (size_t j = i + 1; j < hull.size(); ++j)
			CHECK((hull[i].x != hull[j].x || hull[i].y != hull[j].y));

	const std::vector<ImageGraphCacheBox> repeatedOwner = {{1, 0, 0, 100, 40}, {1, 200, 0, 100, 40}};
	const auto repeatedHull = BuildImageGraphCacheHull(repeatedOwner);
	REQUIRE(repeatedHull.size() >= 3);
	float repeatedRight = repeatedHull.front().x;
	for (const auto point : repeatedHull)
		repeatedRight = std::max(repeatedRight, point.x);
	CHECK(std::fabs(repeatedRight - 254.0f) < 0.001f);
	CHECK(BuildImageGraphCacheHull({}).empty());
	CHECK(BuildImageGraphCacheHull(std::span(boxes).first(1)).empty());
	const std::vector<ImageGraphCacheBox> bad = {
		{1, 0, 0, 10, 10}, {2, std::numeric_limits<float>::quiet_NaN(), 0, 10, 10}
	};
	CHECK(BuildImageGraphCacheHull(bad).empty());
	const std::vector<ImageGraphCacheBox> negative = {{1, 0, 0, 10, 10}, {2, 1, 1, -1, 10}};
	CHECK(BuildImageGraphCacheHull(negative).empty());
}

TEST_CASE(
	"cache background draw reuses graph-space hull and does not mutate graph or replay", "[studio][cache]"
) {
	DrawHarness test;
	const std::string graphBefore = nodegraph::Save(test.Graph);
	const uint64_t signatureBefore = test.Graph.Signature();
	const uint64_t ownerHash = test.Graph.Hash(test.Owner), memberHash = test.Graph.Hash(test.Member);
	const auto selectionBefore = test.Canvas.Selection();
	const auto groupsBefore = test.Groups;
	test.Frame();
	REQUIRE(test.DrawAccepted);
	REQUIRE(test.Backgrounds.Shapes.size() == 1);
	auto &shape = test.Backgrounds.Shapes.front();
	REQUIRE(shape.Boxes.size() == 4);
	CHECK(shape.Boxes[0].Id == test.Owner);
	CHECK(shape.Boxes[1].Id == test.Owner);
	CHECK(shape.Boxes[2].Id == test.Member);
	CHECK(shape.Boxes[3].Id == test.Member);
	REQUIRE(shape.Hull.size() >= 3);
	const auto originalHull = shape.Hull;
	const auto *originalHullData = shape.Hull.data();
	const auto originalHullCapacity = shape.Hull.capacity();
	const ImU32 fill = ImGui::ColorConvertFloat4ToU32({1, 1, 1, 0.025f});
	const ImU32 border = ImGui::ColorConvertFloat4ToU32({1, 1, 1, 0.3f});
	const int fillAt = FirstVertexWithColor(fill);
	const int borderAt = FirstVertexWithColor(border);
	const int bodyAt = FirstVertexWithColor(test.Canvas.Look.NodeBody);
	CHECK(fillAt >= 0);
	CHECK(borderAt >= 0);
	CHECK(bodyAt >= 0);
	CHECK(fillAt < bodyAt);
	CHECK(borderAt < bodyAt);
	CHECK(test.Graph.Signature() == signatureBefore);
	CHECK(nodegraph::Save(test.Graph) == graphBefore);
	CHECK(test.Graph.Hash(test.Owner) == ownerHash);
	CHECK(test.Graph.Hash(test.Member) == memberHash);
	CHECK(test.Canvas.Selection() == selectionBefore);
	CHECK(test.Groups == groupsBefore);

	test.Canvas.SetZoom(1.5f);
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(test.Backgrounds.Shapes.front().Hull.data() == originalHullData);
	CHECK(test.Backgrounds.Shapes.front().Hull.capacity() == originalHullCapacity);
	CHECK(SamePoints(test.Backgrounds.Shapes.front().Hull, originalHull));
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(250.0f, 200.0f);
	test.Frame();
	io.AddMouseWheelEvent(0.0f, 1.0f);
	const auto *beforeWheelHull = test.Backgrounds.Shapes.front().Hull.data();
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(test.Canvas.Zoom() > 1.5f);
	CHECK(test.LastView.Scale == test.Canvas.Zoom());
	CHECK(test.Backgrounds.Shapes.front().Hull.data() == beforeWheelHull);

	test.Graph.Find(test.Member)->X += 35.0f;
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(test.Backgrounds.Shapes.front().Boxes[2].X == test.Graph.Find(test.Member)->X);
	CHECK_FALSE(SamePoints(test.Backgrounds.Shapes.front().Hull, originalHull));

	const float priorWidth = test.Backgrounds.Shapes.front().Boxes[0].Width;
	const auto priorSizeHull = test.Backgrounds.Shapes.front().Hull;
	RegisterBackgroundNode(170.0f);
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(test.Backgrounds.Shapes.front().Boxes[0].Width != priorWidth);
	CHECK_FALSE(SamePoints(test.Backgrounds.Shapes.front().Hull, priorSizeHull));

	const auto beforeMembership = nodegraph::Save(test.Graph);
	const auto priorMembershipHull = test.Backgrounds.Shapes.front().Hull;
	const auto added = test.Graph.Add("fixture.cache-background", 300, 80);
	test.Ids.emplace("other", added);
	test.Groups.Owners[0].Members.push_back("other");
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(test.Backgrounds.Shapes.front().Boxes.size() == 5);
	CHECK_FALSE(SamePoints(test.Backgrounds.Shapes.front().Hull, priorMembershipHull));
	CHECK(nodegraph::Save(test.Graph) != beforeMembership);
}

TEST_CASE(
	"cache backgrounds cull from owner depth and remove empty or stale derived shapes", "[studio][cache]"
) {
	DrawHarness test;
	test.Frame();
	REQUIRE(test.DrawAccepted);
	REQUIRE(test.Backgrounds.Shapes.size() == 1);
	CHECK(test.Backgrounds.Shapes.front().Boxes[2].Id == test.Member);
	CHECK(test.Graph.Find(test.Member)->Owner != test.Canvas.Inside());

	test.Graph.Find(test.Owner)->X = -10000.0f;
	test.Graph.Find(test.Member)->X = 40.0f;
	test.Graph.Find(test.Member)->Owner = nodegraph::NO_NODE;
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(FirstVertexWithColor(ImGui::ColorConvertFloat4ToU32({1, 1, 1, 0.025f})) == -1);
	CHECK(FirstVertexWithColor(test.Canvas.Look.NodeBody) >= 0);

	test.Graph.Find(test.Owner)->X = 30.0f;
	test.Graph.Find(test.Owner)->Owner = 700;
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(FirstVertexWithColor(ImGui::ColorConvertFloat4ToU32({1, 1, 1, 0.025f})) == -1);
	test.Graph.Find(test.Owner)->Owner = nodegraph::NO_NODE;
	test.Groups.Owners[0].Members.clear();
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(test.Backgrounds.Shapes.empty());
}

TEST_CASE(
	"cache background cap refusals clear only derived shapes, including owner shrink", "[studio][cache]"
) {
	DrawHarness test;
	const auto secondOwner = test.Graph.Add("fixture.cache-background", 250, 30);
	const auto secondMember = test.Graph.Add("fixture.cache-background", 380, 30);
	test.Ids.emplace("cache2", secondOwner);
	test.Ids.emplace("producer2", secondMember);
	test.Groups.Owners.push_back({"cache2", true, {"producer2"}});
	const auto authoredGraph = nodegraph::Save(test.Graph);
	const uint64_t signatureBefore = test.Graph.Signature();
	const auto selectionBefore = test.Canvas.Selection();
	test.Frame();
	REQUIRE(test.DrawAccepted);
	REQUIRE(test.Backgrounds.Shapes.size() == 2);
	const uint64_t resident = ResidentBytes(test.Backgrounds);
	test.Groups.Owners.pop_back();
	const auto shrunkenGroups = test.Groups;
	test.MaximumBytes = resident;
	test.Frame();
	CHECK_FALSE(test.DrawAccepted);
	CHECK(test.Error.Code == Status::LimitExceeded);
	CHECK(test.Backgrounds.Shapes.empty());
	CHECK(test.Groups == shrunkenGroups);
	CHECK(test.Graph.Signature() == signatureBefore);
	CHECK(nodegraph::Save(test.Graph) == authoredGraph);
	CHECK(test.Canvas.Selection() == selectionBefore);

	test.MaximumBytes = 1;
	test.Frame();
	CHECK_FALSE(test.DrawAccepted);
	CHECK(test.Backgrounds.Shapes.empty());
	CHECK(test.Groups == shrunkenGroups);

	test.MaximumBytes = 64u * 1024u * 1024u;
	test.Groups.Owners.resize(size_t(Limits::MaximumNodes) + 1);
	const auto oversizedGroups = test.Groups;
	test.Frame();
	CHECK_FALSE(test.DrawAccepted);
	CHECK(test.Error.Code == Status::LimitExceeded);
	CHECK(test.Backgrounds.Shapes.empty());
	CHECK(test.Graph.Signature() == signatureBefore);
	CHECK(nodegraph::Save(test.Graph) == authoredGraph);
	CHECK(test.Canvas.Selection() == selectionBefore);
	CHECK(test.Groups == oversizedGroups);
}

TEST_CASE("forced cache membership refresh keeps other owners and replay intact", "[studio][cache]") {
	DrawHarness test;
	const auto secondOwner = test.Graph.Add("fixture.cache-background", 250, 30);
	test.Ids.emplace("cache2", secondOwner);
	test.Groups.Owners.push_back({"cache2", false, {"producer"}});
	const auto groups = test.Groups;
	const auto saved = nodegraph::Save(test.Graph);
	const auto selection = test.Canvas.Selection();
	test.Frame();
	REQUIRE(test.DrawAccepted);
	REQUIRE(test.Backgrounds.Shapes.size() == 2);
	const auto original = test.Backgrounds.Shapes[0].Hull;
	const auto *unrelated = test.Backgrounds.Shapes[1].Hull.data();
	test.Backgrounds.Invalidate(test.Owner);
	CHECK(test.Backgrounds.Shapes[0].Boxes.empty());
	CHECK(test.Backgrounds.Shapes[0].Hull.empty());
	CHECK(test.Backgrounds.Shapes[1].Hull.data() == unrelated);
	REQUIRE_FALSE(test.Backgrounds.Shapes[1].Hull.empty());
	test.Frame();
	REQUIRE(test.DrawAccepted);
	CHECK(test.Backgrounds.Shapes[0].Owner == test.Owner);
	CHECK(SamePoints(test.Backgrounds.Shapes[0].Hull, original));
	CHECK(test.Backgrounds.Shapes[1].Owner == secondOwner);
	CHECK(test.Backgrounds.Shapes[1].Hull.data() == unrelated);
	CHECK(test.Groups == groups);
	CHECK(nodegraph::Save(test.Graph) == saved);
	CHECK(test.Canvas.Selection() == selection);
}

TEST_CASE("empty graph replacement still admits old cache lookup workspace", "[studio][cache]") {
	DrawHarness test;
	test.Frame();
	REQUIRE(test.DrawAccepted);
	REQUIRE_FALSE(test.Backgrounds.Shapes.empty());
	const auto resident = ResidentBytes(test.Backgrounds);
	test.Graph = nodegraph::Graph{};
	test.Ids.clear();
	test.Groups = {};
	test.Canvas.Select(nodegraph::NO_NODE);
	const auto graph = nodegraph::Save(test.Graph);
	const auto groups = test.Groups;
	test.MaximumBytes = resident;
	test.Frame();
	CHECK_FALSE(test.DrawAccepted);
	CHECK(test.Error.Code == Status::LimitExceeded);
	CHECK(test.Backgrounds.Shapes.empty());
	CHECK(test.Groups == groups);
	CHECK(nodegraph::Save(test.Graph) == graph);
	CHECK(test.Canvas.Selection().empty());
}
