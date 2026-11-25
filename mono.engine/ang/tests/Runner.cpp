#include "Fixture.hpp"

#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>

#include <mutex>
#include <set>

TEST_SUITE_ID("engine.ang.runner")
TEST_DEPENDS("engine.ang.compile")
TEST_TIER("fast")

using namespace engine::ang;
using tests::Evaluations;

namespace {
	struct JobPool {
		explicit JobPool(unsigned workers) {
			engine::parallel::Jobs::Start(workers);
		}
		~JobPool() {
			engine::parallel::Jobs::Stop();
		}
	};

	Graph AddChain() {
		Graph graph;
		tests::Add(graph, tests::Node("one", "test.const", {{"value", Value(1.0)}}));
		tests::Add(graph, tests::Node("two", "test.const", {{"value", Value(2.0)}}));
		tests::Add(graph, tests::Node("sum", "test.add"));
		tests::Add(graph, tests::Node("again", "test.add"));
		tests::Wire(graph, "one", "value", "sum", "a");
		tests::Wire(graph, "two", "value", "sum", "b");
		tests::Wire(graph, "sum", "sum", "again", "a");
		return graph;
	}

	double Number(const Runner &runner, std::string_view node, std::string_view port) {
		const Value *value = runner.Output(node, port);
		REQUIRE(value != nullptr);
		return value->AsNumber();
	}

	// Fan of `count` parallel squares summed by one parallel node.
	Graph Fan(size_t count) {
		Graph graph;
		tests::Add(graph, tests::Node("total", "test.sum"));
		for (size_t index = 0; index < count; ++index) {
			const std::string source = "c" + std::to_string(index);
			const std::string square = "s" + std::to_string(index);
			tests::Add(
				graph, tests::Node(source, "test.const", {{"value", Value(static_cast<double>(index))}})
			);
			tests::Add(graph, tests::Node(square, "test.square"));
			tests::Wire(graph, source, "value", square, "in");
			tests::Wire(graph, square, "out", "total", "values");
		}
		return graph;
	}
}

TEST_CASE("a sync run computes, then a rerun is served from cache", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Runner runner(tests::MustCompile(AddChain(), registry));
	const int before = Evaluations();
	const RunReport first = runner.Run();
	CHECK(first.Ran == 4);
	CHECK(first.Complete);
	CHECK(Number(runner, "again", "sum") == 3.0);
	CHECK(Evaluations() - before == 4);
	const RunReport second = runner.Run();
	CHECK(second.Cached == 4);
	CHECK(second.Ran == 0);
	CHECK(Evaluations() - before == 4);
	CHECK(runner.Output("again", "missing") == nullptr);
	CHECK(runner.Output("nobody", "sum") == nullptr);
}

TEST_CASE("a param edit reruns only the dirty closure", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph = AddChain();
	Runner runner(tests::MustCompile(graph, registry));
	runner.Run();
	REQUIRE(graph.Apply(edit::SetParam{"one", "value", Value(10.0)}).Ok);
	runner.SetPlan(tests::MustCompile(graph, registry));
	const RunReport report = runner.Run();
	CHECK(report.Ran == 3);
	CHECK(report.Cached == 1);
	CHECK(report.Nodes[static_cast<size_t>(runner.CurrentPlan().Find("two"))].Status == NodeStatus::Cached);
	CHECK(Number(runner, "again", "sum") == 12.0);
}

TEST_CASE("run inputs feed unlinked ports, and missing or mistyped ones are reported", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("add", "test.add"));
	Runner runner(tests::MustCompile(graph, registry));
	const RunReport missing = runner.Run();
	CHECK(missing.Skipped == 1);
	CHECK(missing.Nodes[0].Note.find("'a'") != std::string::npos);
	const RunReport supplied = runner.Run({{"add", {{"a", Value(4)}, {"b", Value(0.5)}}}});
	CHECK(supplied.Ran == 1);
	CHECK(Number(runner, "add", "sum") == 4.5);
	// Different inputs are a different cache key.
	CHECK(runner.Run({{"add", {{"a", Value(1.0)}}}}).Ran == 1);
	CHECK(runner.Run({{"add", {{"a", Value(1.0)}}}}).Cached == 1);
	const RunReport mistyped = runner.Run({{"add", {{"a", Value("four")}}}});
	CHECK(mistyped.Failed == 1);
	CHECK(mistyped.Nodes[0].Note.find("expects 'number'") != std::string::npos);
}

TEST_CASE("conversions run on the link and generics pass values through", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("int", "test.int"));
	tests::Add(graph, tests::Node("add", "test.add"));
	tests::Add(graph, tests::Node("text", "test.text"));
	tests::Add(graph, tests::Node("pass", "test.pass"));
	tests::Wire(graph, "int", "value", "add", "a");
	tests::Wire(graph, "text", "text", "pass", "in");
	Runner runner(tests::MustCompile(graph, registry));
	runner.Run();
	CHECK(Number(runner, "add", "sum") == 7.0);
	REQUIRE(runner.Output("pass", "out") != nullptr);
	CHECK(*runner.Output("pass", "out")->Get<std::string>() == "hello");
}

TEST_CASE("failures, unknown types and bypass report their own status", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("fail", "test.fail"));
	tests::Add(graph, tests::Node("after", "test.add"));
	tests::Add(graph, tests::Node("ghost", "plugin.missing"));
	tests::Add(graph, tests::Node("one", "test.const", {{"value", Value(6.0)}}));
	NodeInstance bypassed = tests::Node("skip", "test.add", {});
	bypassed.Bypassed = true;
	tests::Add(graph, bypassed);
	tests::Wire(graph, "fail", "out", "after", "a");
	tests::Wire(graph, "one", "value", "skip", "a");
	tests::Wire(graph, "one", "value", "skip", "b");
	Runner runner(tests::MustCompile(graph, registry));
	const RunReport report = runner.Run();
	const Plan &plan = runner.CurrentPlan();
	CHECK(report.Nodes[static_cast<size_t>(plan.Find("fail"))].Status == NodeStatus::Failed);
	CHECK(report.Nodes[static_cast<size_t>(plan.Find("fail"))].Note == "always fails");
	CHECK(report.Nodes[static_cast<size_t>(plan.Find("after"))].Status == NodeStatus::Skipped);
	CHECK(report.Nodes[static_cast<size_t>(plan.Find("ghost"))].Status == NodeStatus::Skipped);
	CHECK(report.Nodes[static_cast<size_t>(plan.Find("skip"))].Status == NodeStatus::Bypassed);
	// Bypass forwards `a` rather than adding `a + b`.
	CHECK(Number(runner, "skip", "sum") == 6.0);
	CHECK(report.Failed == 1);
	CHECK(report.Skipped == 2);
	CHECK(report.Bypassed == 1);
}

TEST_CASE("impure nodes rerun every time and dirty what reads them", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("count", "test.counter"));
	tests::Add(graph, tests::Node("add", "test.add"));
	tests::Wire(graph, "count", "count", "add", "a");
	Runner runner(tests::MustCompile(graph, registry));
	CHECK(runner.Run().Ran == 2);
	const double first = Number(runner, "add", "sum");
	CHECK(runner.Run().Ran == 2);
	CHECK(Number(runner, "add", "sum") != first);
}

TEST_CASE("a parallel wave gives the serial result", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	const Plan plan = tests::MustCompile(Fan(64), registry, Domain::Tick);
	double serial = 0.0;
	{
		Runner runner(plan);
		CHECK(runner.Run().Ran == 129);
		serial = Number(runner, "total", "sum");
	}
	double expected = 0.0;
	for (int index = 0; index < 64; ++index) {
		expected += index * index;
	}
	CHECK(serial == expected);
	JobPool pool(4);
	Runner runner(plan);
	std::mutex lock;
	std::set<uint64_t> threads;
	runner.Callbacks().PreCompute = [&](const NodeInfo &) {
		std::lock_guard guard(lock);
		threads.insert(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	};
	CHECK(runner.Run().Ran == 129);
	CHECK(Number(runner, "total", "sum") == serial);
	CHECK(!threads.empty());
}

TEST_CASE("async nodes never block and land on a later run", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("slow", "test.slow"));
	tests::Add(graph, tests::Node("after", "test.add"));
	tests::Wire(graph, "slow", "out", "after", "a");
	AsyncPool pool(2);
	Runner runner(tests::MustCompile(graph, registry), {.Pool = &pool});
	const RunReport first = runner.Run();
	CHECK(first.Waiting == 2);
	CHECK(!first.Complete);
	CHECK(runner.Output("after", "sum") == nullptr);
	pool.Drain();
	const RunReport second = runner.Run();
	CHECK(second.Ran == 2);
	CHECK(second.Complete);
	CHECK(Number(runner, "after", "sum") == 10.0);
	CHECK(runner.Run().Cached == 2);
	CHECK(runner.InFlight() == 0);
}

TEST_CASE("an edit cancels in-flight async work for the changed node", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("slow", "test.slow"));
	// One worker, blocked, so the job cannot start before the edit lands.
	AsyncPool pool(1);
	std::mutex gate;
	gate.lock();
	REQUIRE(pool.Submit([&] { std::lock_guard hold(gate); }));
	Runner runner(tests::MustCompile(graph, registry), {.Pool = &pool});
	CHECK(runner.Run({{"slow", {{"in", Value(2.0)}}}}).Waiting == 1);
	CHECK(runner.InFlight() == 1);
	// New input, new run hash: the old job is cancelled and a new one queued.
	CHECK(runner.Run({{"slow", {{"in", Value(3.0)}}}}).Waiting == 1);
	gate.unlock();
	pool.Drain();
	const RunReport landed = runner.Run({{"slow", {{"in", Value(3.0)}}}});
	CHECK(landed.Ran == 1);
	CHECK(Number(runner, "slow", "out") == 30.0);
	runner.Run({{"slow", {{"in", Value(4.0)}}}});
	runner.Cancel();
	CHECK(runner.InFlight() == 0);
	pool.Drain();
}

TEST_CASE("hooks pair up and compute hooks fire only when work happens", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("one", "test.const", {{"value", Value(1.0)}}));
	tests::Add(graph, tests::Node("add", "test.add"));
	tests::Wire(graph, "one", "value", "add", "a");
	Runner runner(tests::MustCompile(graph, registry));
	std::vector<std::string> events;
	Hooks &hooks = runner.Callbacks();
	hooks.PreRun = [&](const RunInfo &) { events.push_back("run+"); };
	hooks.PostRun = [&](const RunInfo &info) {
		REQUIRE(info.Report != nullptr);
		events.push_back("run-");
	};
	hooks.PreNode = [&](const NodeInfo &info) { events.push_back("node+" + std::string(info.Id)); };
	hooks.PostNode = [&](const NodeInfo &info, NodeStatus) {
		events.push_back("node-" + std::string(info.Id));
	};
	hooks.PreCompute = [&](const NodeInfo &info) { events.push_back("work+" + std::string(info.Id)); };
	hooks.PostCompute = [&](const NodeInfo &info, ComputeResult &result) {
		events.push_back("work-" + std::string(info.Id));
		// A debug override replaces the value downstream sees.
		if (info.Id == "one") {
			result.Outputs[0] = Value(100.0);
		}
	};
	runner.Run();
	CHECK(
		events == std::vector<std::string>{
					  "run+",
					  "node+one",
					  "work+one",
					  "work-one",
					  "node-one",
					  "node+add",
					  "work+add",
					  "work-add",
					  "node-add",
					  "run-"
				  }
	);
	CHECK(Number(runner, "add", "sum") == 100.0);
	events.clear();
	runner.Run();
	CHECK(events == std::vector<std::string>{"run+", "node+one", "node-one", "node+add", "node-add", "run-"});
}

TEST_CASE("timeline keeps the last runs with one sample per visit", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Runner runner(tests::MustCompile(AddChain(), registry), {.TimelineRuns = 3});
	for (int run = 0; run < 5; ++run) {
		runner.Run();
	}
	const RunTimeline &timeline = runner.Timeline();
	REQUIRE(timeline.Runs().size() == 3);
	CHECK(timeline.Runs().front().RunNumber == 3);
	CHECK(timeline.Latest()->RunNumber == 5);
	CHECK(timeline.Latest()->Samples.size() == 4);
	for (const TimelineSample &sample : timeline.Latest()->Samples) {
		CHECK(sample.EndNanoseconds >= sample.BeginNanoseconds);
		CHECK(sample.Outcome == NodeStatus::Cached);
	}
}

TEST_CASE("the budget evicts least recently used outputs, then refuses", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	for (const char *id : {"a", "b", "c"}) {
		tests::Add(graph, tests::Node(id, "test.image", {{"bytes", Value(1000)}}));
	}
	Runner runner(tests::MustCompile(graph, registry), {.BudgetBytes = 2500});
	const RunReport report = runner.Run();
	CHECK(report.Ran == 3);
	CHECK(report.Budget == 0);
	// `a` was touched first, so it went to make room for `c`.
	CHECK(runner.Output("a", "image") == nullptr);
	CHECK(runner.Output("c", "image") != nullptr);
	CHECK(runner.Memory("c").Current == 1000);
	CHECK(runner.Memory("c").Cached);
	CHECK(runner.RetainedBytes() == 2000);
	CHECK(report.PeakBytes <= 2500);

	Graph huge;
	tests::Add(huge, tests::Node("big", "test.image", {{"bytes", Value(5000)}}));
	tests::Add(huge, tests::Node("pass", "test.pass"));
	Runner refused(tests::MustCompile(huge, registry), {.BudgetBytes = 2500});
	const RunReport over = refused.Run();
	CHECK(over.Budget == 1);
	CHECK(over.Nodes[0].Note.find("budget") != std::string::npos);
	CHECK(refused.RetainedBytes() == 0);
}

TEST_CASE("outputs a downstream node still needs are never evicted mid-run", "[ang][runner]") {
	Registry registry = tests::MakeRegistry();
	REQUIRE(registry
				.DefineNode(
					{.Id = "test.size",
					 .Inputs = {{.Name = "image", .Type = "image"}},
					 .Outputs = {{.Name = "bytes", .Type = "number"}},
					 .Evaluate =
						 [](EvalContext &context) {
							 context.SetOutput(
								 "bytes",
								 Value(static_cast<double>(context.Input("image").Get<Blob>()->Bytes))
							 );
							 return NodeResult::Success();
						 }}
				)
				.Ok);
	Graph graph;
	tests::Add(graph, tests::Node("a", "test.image", {{"bytes", Value(1000)}}));
	tests::Add(graph, tests::Node("b", "test.image", {{"bytes", Value(1000)}}));
	tests::Add(graph, tests::Node("c", "test.image", {{"bytes", Value(1000)}}));
	tests::Add(graph, tests::Node("size", "test.size"));
	tests::Wire(graph, "a", "image", "size", "image");
	Runner runner(tests::MustCompile(graph, registry), {.BudgetBytes = 2500});
	const RunReport report = runner.Run();
	// `a` is pinned for `size`, so `b` is the one evicted for `c`.
	CHECK(report.Budget == 0);
	CHECK(runner.Output("b", "image") == nullptr);
	CHECK(Number(runner, "size", "bytes") == 1000.0);
}

TEST_CASE("a param named like an unlinked input fills it", "[ang][runner]") {
	const Registry registry = tests::MakeRegistry();
	Graph graph;
	tests::Add(graph, tests::Node("add", "test.add", {{"b", Value(2.0)}}));
	Runner runner(tests::MustCompile(graph, registry));
	runner.Run({{"add", {{"a", Value(1.0)}}}});
	CHECK(Number(runner, "add", "sum") == 3.0);
	// A run-supplied value still wins over the param.
	runner.Run({{"add", {{"a", Value(1.0)}, {"b", Value(10.0)}}}});
	CHECK(Number(runner, "add", "sum") == 11.0);
}
