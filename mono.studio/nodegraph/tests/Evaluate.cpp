// Running the graph, not running it twice, and running two things at once.
//
// The async cases are the reason this module does not use `Engine::parallel`.
// `Jobs::For` blocks until done because a tick has to start and finish; these
// nodes span frames on purpose, and the whole feature is that a graph carrying
// something genuinely slow stays editable while it works. What is asserted here
// is exactly that: `Run` returns while the work is still going, nothing is
// published until it finishes, and two branches that do not feed each other
// overlap.

#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <any>
#include <chrono>
#include <nodegraph/Evaluate.hpp>
#include <nodegraph/Graph.hpp>

TEST_SUITE_ID("studio.nodegraph.evaluate")

using namespace nodegraph;
using fixture::RegisterFixtureNodes;

TEST_CASE("evaluation runs in order and caches what has not changed", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId a = graph.Add("number.constant", 0.0f, 0.0f);
	const NodeId b = graph.Add("number.constant", 0.0f, 120.0f);
	const NodeId sum = graph.Add("number.arithmetic", 220.0f, 0.0f);

	graph.Find(a)->Widgets["value"].Number = 2.0;
	graph.Find(b)->Widgets["value"].Number = 5.0;
	REQUIRE(graph.Connect(a, "Out", sum, "A") == LinkResult::Made);
	REQUIRE(graph.Connect(b, "Out", sum, "B") == LinkResult::Made);

	Evaluator runner;
	RunReport report = runner.Run(graph);
	CHECK(report.Evaluated == 3);
	CHECK(report.Cached == 0);

	const std::any *result = runner.Output(sum, "Out");
	REQUIRE(result != nullptr);
	CHECK(std::any_cast<double>(*result) == 7.0);

	// Nothing changed: everything is a hit.
	report = runner.Run(graph);
	CHECK(report.Evaluated == 0);
	CHECK(report.Cached == 3);
	CHECK(runner.WasCached(sum));

	// One edit invalidates exactly what reads it.
	graph.Find(a)->Widgets["value"].Number = 10.0;
	report = runner.Run(graph);
	CHECK(report.Evaluated == 2);
	CHECK(report.Cached == 1);
	CHECK(std::any_cast<double>(*runner.Output(sum, "Out")) == 15.0);

	// **Putting it back is a hit rather than a recompute**, which is the whole
	// reason results are keyed by hash and not by node.
	graph.Find(a)->Widgets["value"].Number = 2.0;
	report = runner.Run(graph);
	CHECK(report.Evaluated == 0);
	CHECK(report.Cached == 3);

	runner.Forget();
	CHECK(runner.Held() == 0);
}

TEST_CASE("a wire beats a knob, and a node with no eval is skipped", "[nodegraph]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId scale = graph.Add("number.constant", 0.0f, 0.0f);
	const NodeId source = graph.Add("field.source", 200.0f, 0.0f);
	const NodeId note = graph.Add("graph.note", 400.0f, 0.0f);

	graph.Find(scale)->Widgets["value"].Number = 12.0;
	graph.Find(source)->Widgets["frequency"].Number = 2.0;
	REQUIRE(graph.Connect(scale, "Out", source, "Frequency") == LinkResult::Made);

	// The wildcard input takes a field even though nothing declares one.
	CHECK(graph.Connect(source, "Out", note, "Anything") == LinkResult::Made);

	Evaluator runner;
	const RunReport report = runner.Run(graph);
	CHECK(report.Evaluated == 2);

	// **Skipped, not waiting.** A node with no evaluation will never produce
	// anything; a waiting one is about to, and the two facts are different.
	CHECK(report.Skipped == 1);

	// The connected value, not the knob: a graph where the wire did nothing
	// would be one where every generator ignored its own inputs.
	const uint64_t wired = graph.Hash(source);
	graph.Find(scale)->Widgets["value"].Number = 3.0;
	CHECK(graph.Hash(source) != wired);
}

TEST_CASE("an async node runs off the caller's thread and reports as it goes", "[nodegraph][async]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId task = graph.Add("task.staged", 0.0f, 0.0f);
	REQUIRE(task != NO_NODE);
	graph.Find(task)->Widgets["seconds"].Number = 0.4;

	Evaluator runner;

	// **The first `Run` returns while the work is still going.** That is the
	// whole difference from a sync node: if this blocked, the editor would stop
	// drawing for as long as the node takes.
	const auto began = std::chrono::steady_clock::now();
	const RunReport first = runner.Run(graph);
	const auto returned = std::chrono::steady_clock::now();

	CHECK(first.Started == 1);
	CHECK(first.Running == 1);
	CHECK(first.Evaluated == 0);
	CHECK(runner.Busy());
	CHECK(std::chrono::duration<double>(returned - began).count() < 0.2);

	// Nothing is published until it finishes. A half-computed result is worse
	// than none, because the cache would hold it under a hash that says it is
	// the answer.
	CHECK(runner.Output(task, "Done") == nullptr);
	CHECK(runner.Status(task).State == NodeState::Running);

	const RunReport last = runner.RunToCompletion(graph);
	CHECK(last.Running == 0);
	CHECK(!runner.Busy());

	REQUIRE(runner.Output(task, "Done") != nullptr);
	CHECK(std::any_cast<double>(*runner.Output(task, "Done")) == 1.0);

	const NodeStatus status = runner.Status(task);
	CHECK(status.State == NodeState::Done);
	CHECK(status.Progress == 1.0f);
	CHECK(status.Milliseconds > 0.0);

	// And it caches like everything else: the second run recomputes nothing.
	const RunReport again = runner.Run(graph);
	CHECK(again.Cached == 1);
	CHECK(again.Started == 0);
}

TEST_CASE("two branches that do not feed each other run at once", "[nodegraph][async]") {
	RegisterFixtureNodes();
	Graph graph;

	// **Half a second each, and the assertion has a wide margin.** What is being
	// checked is that these overlap at all - a scheduler that ran them one after
	// the other would take twice as long, and the bound is far enough from both
	// numbers that a slow machine does not decide the outcome.
	const NodeId left = graph.Add("task.staged", 0.0f, 0.0f);
	const NodeId right = graph.Add("task.staged", 0.0f, 200.0f);
	graph.Find(left)->Widgets["seconds"].Number = 0.5;
	graph.Find(right)->Widgets["seconds"].Number = 0.5;

	// **Different labels, so they are two hashes.** Identical nodes are one
	// piece of work by design - the evaluator makes the second wait on the
	// first - which is right, and would make this case measure nothing.
	graph.Find(left)->Widgets["label"].Text = "left";
	graph.Find(right)->Widgets["label"].Text = "right";

	Evaluator runner;

	const auto began = std::chrono::steady_clock::now();
	const RunReport report = runner.RunToCompletion(graph);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

	CHECK(report.Running == 0);
	CHECK(runner.Output(left, "Done") != nullptr);
	CHECK(runner.Output(right, "Done") != nullptr);

	INFO("took " << seconds << "s");
	CHECK(seconds < 0.85);
}

TEST_CASE("a node waits for an input that is still being computed", "[nodegraph][async]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId task = graph.Add("task.staged", 0.0f, 0.0f);
	const NodeId doubled = graph.Add("number.arithmetic", 240.0f, 0.0f);
	graph.Find(task)->Widgets["seconds"].Number = 0.4;
	graph.Find(doubled)->Widgets["op"].Text = "add";
	REQUIRE(graph.Connect(task, "Done", doubled, "A") == LinkResult::Made);

	Evaluator runner;
	const RunReport first = runner.Run(graph);

	// **Waiting, not evaluated.** Running it now would read the unconnected
	// fallback - zero - and cache that under a hash which says the input was the
	// task's result. The wrong answer would then be permanent.
	CHECK(first.Waiting == 1);
	CHECK(first.Evaluated == 0);
	CHECK(runner.Output(doubled, "Out") == nullptr);

	const RunReport last = runner.RunToCompletion(graph);
	CHECK(last.Waiting == 0);
	REQUIRE(runner.Output(doubled, "Out") != nullptr);

	// One from the task, plus nothing on B.
	CHECK(std::any_cast<double>(*runner.Output(doubled, "Out")) == 1.0);
}

TEST_CASE("an evaluator with work in flight can still be destroyed", "[nodegraph][async]") {
	RegisterFixtureNodes();
	Graph graph;

	const NodeId task = graph.Add("task.staged", 0.0f, 0.0f);
	graph.Find(task)->Widgets["seconds"].Number = 6.0;

	const auto began = std::chrono::steady_clock::now();
	{
		Evaluator runner;
		runner.Run(graph);
		REQUIRE(runner.Busy());
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

	// **The editor closes while something long is running, often.** The task
	// polls `Inputs::Cancelled` between slices, so shutdown costs a slice rather
	// than the six seconds it was asked for.
	INFO("shutdown took " << seconds << "s");
	CHECK(seconds < 1.5);
}
