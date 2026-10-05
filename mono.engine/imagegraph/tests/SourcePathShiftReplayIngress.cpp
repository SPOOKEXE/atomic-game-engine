#include "SourcePathShiftVisit.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_path_shift_replay_ingress")
using namespace engine::imagegraph;
namespace {
	Path2D ReplayLine() {
		Path2D p;
		p.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
		return p;
	}
	Path2D ReplayBlob(uint64_t stamp) {
		Path2D p;
		auto &op = p.SourceOperation.emplace();
		op.Kind = SourcePathOperationKind::Shift;
		op.Inputs = {ReplayLine()};
		op.ShiftDistance = -2;
		op.EvaluationMemoId = stamp;
		return p;
	}
	Document ReplayGraph(std::string route) {
		Document d;
		d.FormatVersion = 9;
		Node collect{"collect", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		collect.DynamicInputs = {
			{"input_0", ValueType::Vector2, std::nullopt}, {"input_1", ValueType::Vector2, std::nullopt}
		};
		d.Nodes = {
			{"live", "pc.path_shift", "", {}, {{"path", ReplayLine()}, {"distance", 2.}}},
			{"live_sample", "pc.path_sample", "", {}, {{"ratio", .5}}},
			{"cache",
			 route,
			 "",
			 {},
			 route == "pc.cache_value_array" ? std::vector<AuthoredValue>{{"stop_frame", int64_t{1}}}
											 : std::vector<AuthoredValue>{{"frames", int64_t{1}}}},
			{"pick", "pc.array_get", "", {}, {{"index", int64_t{0}}}},
			{"old_sample", "pc.path_sample", "", {}, {{"ratio", .5}}},
			std::move(collect)
		};
		d.Links = {
			{"live", "path", "live_sample", "path"},
			{"live", "path", "cache", "value"},
			{"live_sample", "position", "collect", "input_0"},
			{"old_sample", "position", "collect", "input_1"}
		};
		if (route == "pc.cache_value_array") {
			d.Links.push_back({"cache", "cache_array", "pick", "array"});
			d.Links.push_back({"pick", "value", "old_sample", "path"});
		} else
			d.Links.push_back({"cache", "value", "old_sample", "path"});
		d.Outputs = {{"points", "collect", "array"}};
		return d;
	}
	DataReplayState Prior(uint64_t stamp) {
		DataReplayState prior;
		prior.Entries.push_back({"cache", 0, 0, 0, false, true, 0, 0, false, {{0, ReplayBlob(stamp)}}});
		return prior;
	}
	uint64_t Stamp(const DataReplayState &prior) {
		return std::get<Path2D>(prior.Entries[0].Values[0].Data).SourceOperation->EvaluationMemoId;
	}
}
TEST_CASE(
	"Frozen cache-group path snapshots clear evaluation-local identities on replay and Pixel Builder copies",
	"[imagegraph][source_path_shift][frame_cache_groups]"
) {
	DataReplayState prior;
	std::vector<CacheGroupReplayOutput> outputs{{"path", Value{ReplayBlob(77)}, {}, {}}};
	REQUIRE(
		RetainCacheGroupReplayNode(
			prior.CacheGroups, "producer", "pc.path_reverse", outputs, Limits::MaximumEvaluationBytes
		)
			.Code == Status::Ok
	);
	prior.CacheGroups.Nodes.front().RenderActive = false;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"producer", "pc.path_reverse", "", {}, {{"path", ReplayLine()}}}};
	document.Outputs = {{"out", "producer", "path"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	StatefulEvaluationResult result;
	EvaluationRequest request;
	request.DataReplay = &prior;
	REQUIRE(EvaluateStateful(document, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(
		std::get<Path2D>(*prior.CacheGroups.Nodes.front().Outputs.front().Data)
			.SourceOperation->EvaluationMemoId == 77
	);
	CHECK(
		std::get<Path2D>(*result.Data.CacheGroups.Nodes.front().Outputs.front().Data)
			.SourceOperation->EvaluationMemoId == 0
	);
	DynamicSurfaceValue recipe;
	recipe.Data.emplace().DataHistory = prior;
	Value owned = recipe;
	detail::StripSourcePathShiftIdentities(owned);
	const auto &copied = std::get<DynamicSurfaceValue>(owned).Data->DataHistory->CacheGroups;
	CHECK(
		std::get<Path2D>(*copied.Nodes.front().Outputs.front().Data).SourceOperation->EvaluationMemoId == 0
	);
	CHECK(
		std::get<Path2D>(*recipe.Data->DataHistory->CacheGroups.Nodes.front().Outputs.front().Data)
			.SourceOperation->EvaluationMemoId == 77
	);
}
TEST_CASE(
	"Shift cached and delayed prior values enter a fresh memo namespace in both evaluator APIs",
	"[source_path_shift_replay_ingress]"
) {
	for (const std::string route : {"pc.cache_value_array", "pc.delay_value"})
		for (const bool stateful : {false, true})
			for (const uint64_t stamp : {uint64_t{0}, uint64_t{1}, uint64_t{404}}) {
				const auto document = ReplayGraph(route);
				Plan plan;
				Diagnostic error;
				REQUIRE(Compile(document, plan, error) == Status::Ok);
				auto prior = Prior(stamp);
				const auto before = prior;
				EvaluationRequest request;
				request.Tick = 1;
				request.DataReplay = &prior;
				StatefulEvaluationResult output;
				EvaluatedValue ordinary;
				const auto status = stateful
										? EvaluateStateful(document, plan, "points", request, output, error)
										: EvaluateValue(document, plan, "points", request, ordinary, error);
				INFO(route);
				INFO(stamp);
				INFO(stateful);
				INFO(error.Message);
				REQUIRE(status == Status::Ok);
				const auto &array = std::get<ArrayValue>(
					stateful ? std::get<EvaluatedValue>(output.Output).Data : ordinary.Data
				);
				REQUIRE(array.Elements.size() == 2);
				CHECK(std::get<Vector2>(array.Elements[0]) == Vector2{5, -2});
				CHECK(std::get<Vector2>(array.Elements[1]) == Vector2{5, 2});
				CHECK(prior == before);
				CHECK(Stamp(prior) == stamp);
				if (stateful) CHECK(Stamp(output.Data) == 0);
			}
}
TEST_CASE(
	"Shift normalized prior cache copies roll back after a later sample refusal",
	"[source_path_shift_replay_ingress]"
) {
	for (const std::string route : {"pc.cache_value_array", "pc.delay_value"}) {
		auto document = ReplayGraph(route);
		document.Nodes[4].Values.push_back({"path_index", int64_t{-1}});
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(document, plan, error) == Status::Ok);
		auto prior = Prior(1);
		const auto before = prior;
		EvaluationRequest request;
		request.Tick = 1;
		request.DataReplay = &prior;
		StatefulEvaluationResult output;
		output.Output = EvaluatedValue{"old", int64_t{17}, {}};
		output.Data = Prior(77);
		CHECK(EvaluateStateful(document, plan, "points", request, output, error) == Status::InvalidValue);
		CHECK(std::get<int64_t>(std::get<EvaluatedValue>(output.Output).Data) == 17);
		CHECK(Stamp(output.Data) == 77);
		CHECK(prior == before);
		CHECK(Stamp(prior) == 1);
	}
}
TEST_CASE(
	"Shift visitor normalization of a copied recipe preserves borrowed nested group storage",
	"[source_path_shift_replay_ingress]"
) {
	DynamicSurfaceValue surface;
	auto &recipe = surface.Data.emplace();
	recipe.Authored.FormatVersion = 9;
	const auto blob = ReplayBlob(17);
	recipe.Authored.Nodes.push_back({"node", "pc.path_shift", "", {}, {{"path", blob}}});
	recipe.DataHistory = Prior(17);
	recipe.Groups.emplace();
	auto groups = std::make_unique<detail::GroupReplayAccess::Owner>(Limits::MaximumEvaluationBytes);
	GroupReplayEntry entry;
	entry.NodeId = "group";
	entry.ParentReset = blob;
	entry.SubtypeStatic = blob;
	groups->Entries.push_back(std::move(entry));
	groups->SharedSubtypes.push_back({"group", blob, {}, "subtype"});
	detail::GroupReplayAccess::Install(recipe.Groups->Replay, std::move(groups));
	Value prior = std::move(surface), owned = prior;
	detail::StripSourcePathShiftIdentities(owned);
	const auto &a = std::get<DynamicSurfaceValue>(prior), &b = std::get<DynamicSurfaceValue>(owned);
	CHECK(a.Data.operator->() != b.Data.operator->());
	CHECK(
		detail::GroupReplayAccess::Get(a.Data->Groups->Replay) !=
		detail::GroupReplayAccess::Get(b.Data->Groups->Replay)
	);
	size_t originals = 0, cleared = 0;
	detail::SourcePathShiftRoute route;
	auto before = [&](const SourcePathData2D &op, const detail::SourcePathShiftRoute &) {
		CHECK(op.EvaluationMemoId == 17);
		++originals;
		return true;
	};
	auto after = [&](const SourcePathData2D &op, const detail::SourcePathShiftRoute &) {
		CHECK(op.EvaluationMemoId == 0);
		++cleared;
		return true;
	};
	REQUIRE(detail::VisitSourcePathShift(a, route, before));
	REQUIRE(detail::VisitSourcePathShift(b, route, after));
	CHECK(originals == 5);
	CHECK(cleared == 5);
}
