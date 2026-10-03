#include <engine/imagegraph/CacheResultsReplay.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.cache_results_replay")
TEST_DEPENDS("engine.imagegraph.source_cache_results")
using namespace engine::imagegraph;
namespace {
	Document Scene(int64_t amount = 1) {
		Document d;
		d.FormatVersion = 9;
		d.Timeline = TimelineSettings{3, 0, 2, "loop", 24};
		d.Project = ProjectSettings{};
		d.Project->SurfaceWidth = 2;
		d.Project->SurfaceHeight = 1;
		d.Groups = {{"A", "first scope"}, {"B", "second scope"}};
		d.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{10, 0, 0, 255}}}},
			{"cacheA", "pc.cache_results", "A", {}, {{"amount", amount}}},
			{"cacheB", "pc.cache_results", "B", {}, {{"amount", int64_t{2}}}},
			{"selectA", "pc.sequence_anim", "A", {}, {{"overflow", EnumValue{0}}}},
			{"selectB", "pc.sequence_anim", "B", {}, {{"overflow", EnumValue{0}}}},
			{"feedbackA", "image.captured", "", {}, {{"source_id", std::string("feedback:A-image")}}},
			{"feedbackB", "image.captured", "", {}, {{"source_id", std::string("feedback:B-image")}}}
		};
		d.Links = {
			{"source", "image", "cacheA", "surface_in"},
			{"source", "image", "cacheB", "surface_in"},
			{"cacheA", "cache_surfaces", "selectA", "surface_in"},
			{"cacheB", "cache_surfaces", "selectB", "surface_in"}
		};
		d.Outputs = {
			{"A-image", "selectA", "surface_out"},
			{"B-image", "selectB", "surface_out"},
			{"A-list", "cacheA", "cache_surfaces"},
			{"B-list", "cacheB", "cache_surfaces"}
		};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diagnostic;
		const auto status = Compile(d, p, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	void Prepared(
		CapturedFeedbackHost &host,
		const Document &d,
		const Plan &p,
		EvaluationRequest &request,
		std::string_view selected = "A-image"
	) {
		Diagnostic diagnostic;
		const bool ok =
			host.Prepare(d, p, 17, 29, request, diagnostic, Limits::MaximumEvaluationBytes, selected);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(ok);
	}
	const DataReplayEntry &Entry(const DataReplayState &state, std::string_view id) {
		for (const auto &entry : state.Entries)
			if (entry.NodeId == id) return entry;
		FAIL("missing replay row");
		return state.Entries.front();
	}
	const ArrayValue &Slots(const DataReplayEntry &entry) {
		REQUIRE(entry.Values.size() == 1);
		return std::get<ArrayValue>(entry.Values[0].Data);
	}
}
TEST_CASE(
	"Cache Results clear commits only selected source slots and dependent preview invalidation",
	"[cache_results_clear]"
) {
	const auto d = Scene();
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	Prepared(host, d, p, request);
	const auto before = *request.DataReplay;
	const auto simulation = *request.SimulationReplay;
	const auto surfaces = *request.SurfaceReplay;
	const auto random = *request.RandomReplay;
	const auto rigid = *request.RigidReplay;
	const Image other = *host.Output("B-image");
	const auto sources =
		std::vector<RequestImageSource>(request.ImageSources.begin(), request.ImageSources.end());
	Diagnostic diagnostic;
	REQUIRE(host.ClearCacheResults(d, p, "cacheA", 17, 29, diagnostic));
	CHECK(Slots(Entry(*request.DataReplay, "cacheA")).ElementType == ValueType::Struct);
	CHECK(host.Value("A-image") == nullptr);
	CHECK(host.Value("A-list") == nullptr);
	REQUIRE(host.Output("B-image"));
	CHECK(*host.Output("B-image") == other);
	Prepared(host, d, p, request, "B-image");
	CHECK(Entry(*request.DataReplay, "cacheB") == Entry(before, "cacheB"));
	// Both feedback-bound outputs belong to one preview closure: no same-clock rerun is allowed.
	CHECK_FALSE(host.Prepare(d, p, 17, 29, request, diagnostic, Limits::MaximumEvaluationBytes, "A-image"));
	CHECK(diagnostic.Code == Status::UnsupportedExecution);
	CHECK(*request.SimulationReplay == simulation);
	CHECK(*request.SurfaceReplay == surfaces);
	CHECK(*request.RandomReplay == random);
	CHECK(*request.RigidReplay == rigid);
	REQUIRE(request.ImageSources.size() == sources.size());
	for (size_t i = 0; i < sources.size(); ++i) {
		CHECK(request.ImageSources[i].SourceId == sources[i].SourceId);
		CHECK(request.ImageSources[i].Data == sources[i].Data);
	}
	CHECK(Entry(*request.DataReplay, "cacheB") == Entry(before, "cacheB"));
	const auto &cleared = Entry(*request.DataReplay, "cacheA");
	CHECK(cleared.Tick == Entry(before, "cacheA").Tick);
	CHECK(cleared.Subframe == Entry(before, "cacheA").Subframe);
	CHECK(cleared.PreviousValue == 0);
	REQUIRE(Slots(cleared).Elements.size() == 1);
	CHECK(IsFreedCacheResultsSlot(Slots(cleared).Elements[0]));
	const auto clearAgain = *request.DataReplay;
	REQUIRE(host.ClearCacheResults(d, p, "cacheA", 17, 29, diagnostic));
	CHECK(*request.DataReplay == clearAgain);
	CHECK(std::get<SurfaceValue>(Slots(Entry(before, "cacheA")).Elements[0]).Data.Pixels[0] == 10);
}
TEST_CASE(
	"Cache Results clear refuses stale or wrong scoped identities and low admission atomically",
	"[cache_results_clear]"
) {
	const auto d = Scene();
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	Prepared(host, d, p, request);
	const auto before = *request.DataReplay;
	const Image good = *host.Output("A-image");
	Diagnostic diagnostic;
	for (const auto id : {"cache", "A", "source"}) {
		CHECK_FALSE(host.ClearCacheResults(d, p, id, 17, 29, diagnostic));
		CHECK(diagnostic.Code == Status::UnknownNode);
	}
	CHECK_FALSE(host.ClearCacheResults(d, p, "cacheA", 18, 29, diagnostic));
	CHECK_FALSE(host.ClearCacheResults(d, p, "cacheA", 17, 30, diagnostic));
	CHECK_FALSE(host.ClearCacheResults(d, p, "cacheA", 17, 29, diagnostic, 1));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(*request.DataReplay == before);
	REQUIRE(host.Output("A-image"));
	CHECK(*host.Output("A-image") == good);
	Prepared(host, d, p, request);
	CHECK(*request.DataReplay == before);
}
TEST_CASE(
	"Cache Results freed list retains length and explicit invalid-slot diagnostic", "[cache_results_clear]"
) {
	auto d = Scene(2);
	// Direct DataReplay observations allow two exact first-frame source calls.
	d.Nodes.erase(d.Nodes.end() - 2, d.Nodes.end());
	d.Outputs = {{"out", "cacheA", "cache_surfaces"}};
	const auto p = Compiled(d);
	EvaluationRequest request;
	StatefulEvaluationResult result;
	Diagnostic diagnostic;
	REQUIRE(EvaluateStateful(d, p, "out", request, result, diagnostic) == Status::Ok);
	request.DataReplay = &result.Data;
	REQUIRE(EvaluateStateful(d, p, "out", request, result, diagnostic) == Status::Ok);
	DataReplayState cleared;
	REQUIRE(ClearCacheResultsReplay(result.Data, "cacheA", cleared, diagnostic) == Status::Ok);
	const auto retained = cleared;
	REQUIRE(Slots(Entry(cleared, "cacheA")).Elements.size() == 2);
	for (const auto &slot : Slots(Entry(cleared, "cacheA")).Elements)
		CHECK(IsFreedCacheResultsSlot(slot));
	request.Tick = 1;
	request.Subframe = .5;
	request.NegativeFrame = true;
	request.DataReplay = &cleared;
	const auto pixels = std::get<ImageArray>(result.Output).Images;
	CHECK(EvaluateStateful(d, p, "out", request, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Port == "cache_surfaces");
	CHECK(cleared == retained);
	CHECK(std::get<ImageArray>(result.Output).Images == pixels);
	// Shrink then rotate an exact full one-slot list removes the invalid position.
	d.Nodes[1].Values[0].Data = int64_t{1};
	request.Tick = 0;
	request.Subframe = 0;
	request.NegativeFrame = false;
	REQUIRE(EvaluateStateful(d, Compiled(d), "out", request, result, diagnostic) == Status::Ok);
	REQUIRE(Slots(Entry(result.Data, "cacheA")).Elements.size() == 1);
	CHECK(std::holds_alternative<SurfaceValue>(Slots(Entry(result.Data, "cacheA")).Elements[0]));
	CHECK(cleared == retained);
}
TEST_CASE("Cache Results clear survives native restart and bounded backward seeks", "[cache_results_clear]") {
	const auto d = Scene();
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	Prepared(host, d, p, request);
	Diagnostic diagnostic;
	REQUIRE(host.ClearCacheResults(d, p, "cacheA", 17, 29, diagnostic));
	const auto cleared = *request.DataReplay;
	host.RestartCycle();
	CHECK_FALSE(host.Prepare(d, p, 17, 29, request, diagnostic, Limits::MaximumEvaluationBytes, "A-image"));
	CHECK(*request.DataReplay == cleared);
	request.Tick = 1;
	Prepared(host, d, p, request);
	REQUIRE(host.Output("A-image"));
	CHECK(host.Output("A-image")->Pixels[0] == 10);
	CHECK(Slots(Entry(*request.DataReplay, "cacheA")).Elements.size() == 1);
	REQUIRE(host.ClearCacheResults(d, p, "cacheA", 17, 29, diagnostic));
	request.Tick = 0;
	request.Subframe = .5;
	request.NegativeFrame = true;
	// This owner includes feedback bindings; fractional negative seeks are unsupported.
	const auto beforeSeek = *request.DataReplay;
	CHECK_FALSE(host.Prepare(d, p, 17, 29, request, diagnostic, Limits::MaximumEvaluationBytes, "A-image"));
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(*request.DataReplay == beforeSeek);
	request.Subframe = 0;
	request.NegativeFrame = false;
	// A valid backward seek retains the clear event and reconstructs one source slot.
	Prepared(host, d, p, request);
	CHECK(Slots(Entry(*request.DataReplay, "cacheA")).Elements.size() == 1);
	CHECK_FALSE(Entry(*request.DataReplay, "cacheA").NegativeFrame);
	CHECK(Entry(*request.DataReplay, "cacheA").Subframe == 0);
	CHECK(Entry(*request.DataReplay, "cacheA").Tick == 0);
	REQUIRE(host.Output("A-image"));
	CHECK(host.Output("A-image")->Pixels[0] == 10);
}

TEST_CASE(
	"Cache Results seek overlays a Clear once before replaying later first-frame cycles",
	"[cache_results_clear]"
) {
	const auto build = [] {
		auto d = Scene(3);
		d.Timeline->First = 1;
		return d;
	};
	const auto d = build();
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	Prepared(host, d, p, request);
	REQUIRE(Slots(Entry(*request.DataReplay, "cacheA")).Elements.size() == 1);
	Diagnostic diagnostic;
	REQUIRE(host.ClearCacheResults(d, p, "cacheA", 17, 29, diagnostic));
	request.Tick = 2;
	Prepared(host, d, p, request);
	const auto &row = Entry(*request.DataReplay, "cacheA");
	CHECK(row.PreviousValue == 1);
	REQUIRE(Slots(row).Elements.size() == 2);
	for (const auto &slot : Slots(row).Elements)
		CHECK(std::holds_alternative<SurfaceValue>(slot));
}

TEST_CASE(
	"Cache Results clear helper admits replacement overlap and retains prior output on refusal",
	"[cache_results_clear]"
) {
	const auto d = Scene();
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	Prepared(host, d, p, request);
	const auto source = *request.DataReplay;
	DataReplayState output = source;
	const auto original = output;
	Diagnostic diagnostic;
	const uint64_t cap = RetainedDataReplayBytes(source) + ClearedCacheResultsReplayBytes(source, "cacheA") +
						 source.Entries.size() * sizeof(size_t);
	CHECK(ClearCacheResultsReplay(source, "cacheA", output, diagnostic, cap) == Status::LimitExceeded);
	CHECK(output == original);
	CHECK(source == original);
	const uint64_t withOutput = cap + RetainedDataReplayBytes(output) - sizeof(DataReplayState);
	REQUIRE(ClearCacheResultsReplay(source, "cacheA", output, diagnostic, withOutput) == Status::Ok);
	CHECK(IsFreedCacheResultsSlot(Slots(Entry(output, "cacheA")).Elements.front()));
	CHECK(Entry(output, "cacheB") == Entry(original, "cacheB"));
	CHECK(source == original);
}

TEST_CASE(
	"Cache Results clear preserves processor identity and refuses malformed freed-slot metadata",
	"[cache_results_clear]"
) {
	const auto d = Scene();
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	Prepared(host, d, p, request);
	auto source = *request.DataReplay;
	auto inactive = Entry(source, "cacheA");
	inactive.ProcessorRow = 7;
	inactive.PreviousValue = 3;
	inactive.Subframe = .25;
	inactive.PreviousFrame = .25;
	source.Entries.push_back(inactive);
	const auto prior = source;
	DataReplayState cleared;
	Diagnostic diagnostic;
	REQUIRE(ClearCacheResultsReplay(source, "cacheA", cleared, diagnostic) == Status::Ok);
	CHECK(cleared.Entries.size() == source.Entries.size());
	for (const auto &row : cleared.Entries)
		if (row.NodeId == "cacheA") {
			CHECK(row.PreviousValue == 0);
			CHECK(IsFreedCacheResultsSlot(Slots(row).Elements.front()));
			if (row.ProcessorRow == 7) CHECK(row.Subframe == .25);
		}
	CHECK(source == prior);
	auto &marker = std::get<StructValue>(std::get<ArrayValue>(cleared.Entries[0].Values[0].Data).Elements[0]);
	const auto validCleared = cleared;
	marker.Data->Fields[0].second = false;
	DataReplayState output = prior;
	CHECK(ClearCacheResultsReplay(cleared, "cacheA", output, diagnostic) == Status::InvalidValue);
	CHECK(output == prior);
	CHECK(IsFreedCacheResultsSlot(Slots(Entry(validCleared, "cacheA")).Elements.front()));
}

TEST_CASE(
	"Cache Results Clear survives Amount edits and retires removed or replaced identities",
	"[cache_results_clear]"
) {
	for (const bool replaceType : {false, true}) {
		auto document = Scene(1);
		auto plan = Compiled(document);
		CapturedFeedbackHost host;
		EvaluationRequest request;
		Prepared(host, document, plan, request);
		Diagnostic diagnostic;
		REQUIRE(host.ClearCacheResults(document, plan, "cacheA", 17, 29, diagnostic));
		const auto cleared = *request.DataReplay;
		const auto prepare = [&](uint64_t revision, std::string_view selected = "A-image") {
			const bool ok = host.Prepare(
				document, plan, revision, 29, request, diagnostic, Limits::MaximumEvaluationBytes, selected
			);
			INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
			return ok;
		};
		// Growing before a source first-frame update exposes the freed old slot;
		// an authored control revision must not recreate it as a new empty list.
		document.Nodes[1].Values[0].Data = int64_t{2};
		plan = Compiled(document);
		CHECK_FALSE(prepare(18));
		CHECK(diagnostic.Code == Status::UnsupportedExecution);
		CHECK(*request.DataReplay == cleared);
		// Shrinking to one lets the source first-frame rotation replace every slot.
		document.Nodes[1].Values[0].Data = int64_t{1};
		plan = Compiled(document);
		REQUIRE(prepare(18));
		REQUIRE(host.Output("A-image"));
		CHECK(host.Output("A-image")->Pixels[0] == 10);
		CHECK(Slots(Entry(*request.DataReplay, "cacheA")).Elements.size() == 1);
		document.Nodes[1].Values[0].Data = int64_t{3};
		plan = Compiled(document);
		REQUIRE(prepare(19));
		CHECK(Slots(Entry(*request.DataReplay, "cacheA")).Elements.size() == 2);
		const auto restored = document;
		std::erase_if(document.Links, [](const auto &link) {
			return link.FromNode == "cacheA" || link.ToNode == "cacheA" || link.FromNode == "selectA" ||
				   link.ToNode == "selectA";
		});
		std::erase_if(document.Nodes, [&](const auto &node) {
			return node.Id == "selectA" || node.Id == "feedbackA" || (!replaceType && node.Id == "cacheA");
		});
		std::erase_if(document.Outputs, [](const auto &out) { return out.Id.starts_with("A-"); });
		if (replaceType) {
			for (auto &node : document.Nodes)
				if (node.Id == "cacheA")
					node = {
						"cacheA",
						"image.solid",
						"A",
						{},
						{{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{50, 0, 0, 255}}}
					};
			document.Outputs.push_back({"replacement", "cacheA", "image"});
		}
		plan = Compiled(document);
		REQUIRE(prepare(20, "B-image"));
		CHECK(
			std::none_of(
				request.DataReplay->Entries.begin(), request.DataReplay->Entries.end(), [](const auto &row) {
					return row.NodeId == "cacheA";
				}
			)
		);
		document = restored;
		plan = Compiled(document);
		REQUIRE(prepare(21));
		CHECK(Slots(Entry(*request.DataReplay, "cacheA")).Elements.size() == 1);
		REQUIRE(host.Output("A-image"));
		CHECK(host.Output("A-image")->Pixels[0] == 10);
	}
}

TEST_CASE("Whole replay owner Clear retires manual Cache Results slot history", "[cache_results_clear]") {
	const auto document = Scene(3);
	const auto plan = Compiled(document);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	Prepared(host, document, plan, request);
	Diagnostic diagnostic;
	REQUIRE(host.ClearCacheResults(document, plan, "cacheA", 17, 29, diagnostic));
	REQUIRE(IsFreedCacheResultsSlot(Slots(Entry(*request.DataReplay, "cacheA")).Elements.front()));
	host.Clear();
	Prepared(host, document, plan, request);
	REQUIRE(Slots(Entry(*request.DataReplay, "cacheA")).Elements.size() == 1);
	CHECK(std::holds_alternative<SurfaceValue>(Slots(Entry(*request.DataReplay, "cacheA")).Elements.front()));
	REQUIRE(host.Output("A-image"));
	CHECK(host.Output("A-image")->Pixels[0] == 10);
}

TEST_CASE(
	"Cache Results Clear admits dependency and identifier scans before retiring slots",
	"[cache_results_clear]"
) {
	for (const size_t nameBytes : {size_t{16}, size_t{256}}) {
		auto document = Scene();
		// These valid unvisited nodes keep pixels cheap while exercising the full
		// declared-graph ID search/name bound, independent of the selected cone.
		for (size_t index = 0; index < 1024; ++index) {
			const std::string id = std::string(nameBytes, 'x') + std::to_string(index);
			document.Nodes.push_back(
				{id,
				 "image.solid",
				 "",
				 {},
				 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 255}}}}
			);
		}
		const auto plan = Compiled(document);
		CapturedFeedbackHost host;
		EvaluationRequest request;
		Prepared(host, document, plan, request);
		const auto prior = *request.DataReplay;
		const auto pixels = *host.Output("A-image");
		const auto other = *host.Output("B-image");
		Diagnostic diagnostic;
		CHECK_FALSE(host.ClearCacheResults(document, plan, "cacheA", 17, 29, diagnostic));
		CHECK(diagnostic.Code == Status::LimitExceeded);
		CHECK(diagnostic.Message == "Cache Results clear dependency/name work exceeds bounds");
		CHECK(*request.DataReplay == prior);
		CHECK(host.CacheResultsInvalidatedOutputs().empty());
		REQUIRE(host.Output("A-image"));
		CHECK(*host.Output("A-image") == pixels);
		CHECK(*host.Output("B-image") == other);
		Prepared(host, document, plan, request);
		CHECK(*request.DataReplay == prior);
	}
}
