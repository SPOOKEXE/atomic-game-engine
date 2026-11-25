#pragma once

// A small registry of our own. Never import a consumer's node set here.

#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

namespace engine::ang::tests {

	// Counts how many times Evaluate ran, across threads.
	inline std::atomic<int> &Evaluations() {
		static std::atomic<int> count{0};
		return count;
	}

	struct BlobPayload {
		std::vector<unsigned char> Bytes;
	};

	inline Registry MakeRegistry() {
		Registry registry = Registry::WithScalars();
		registry.DefineType({.Id = "image", .Label = "Image", .Bytes = [](const Value &value) {
								 const Blob *blob = value.Get<Blob>();
								 return blob == nullptr ? size_t{0} : blob->Bytes;
							 }});
		registry.DefineNode(
			{.Id = "test.const",
			 .Outputs = {{.Name = "value", .Type = "number"}},
			 .Params = {{.Name = "value", .Type = "number", .Default = Value(0.0)}},
			 .Evaluate = [](EvalContext &context) {
				 ++Evaluations();
				 context.SetOutput("value", Value(context.Param("value").AsNumber()));
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.add",
			 .Inputs =
				 {{.Name = "a", .Type = "number"}, {.Name = "b", .Type = "number", .Default = Value(0.0)}},
			 .Outputs = {{.Name = "sum", .Type = "number"}},
			 .Evaluate =
				 [](EvalContext &context) {
					 ++Evaluations();
					 context.SetOutput(
						 "sum", Value(context.Input("a").AsNumber() + context.Input("b").AsNumber())
					 );
					 return NodeResult::Success();
				 },
			 .Bypass = {{.Input = "a", .Output = "sum"}}}
		);
		registry.DefineNode(
			{.Id = "test.sum",
			 .Inputs = {{.Name = "values", .Type = "number", .MaximumLinks = 0}},
			 .Outputs = {{.Name = "sum", .Type = "number"}},
			 .Mode = ExecMode::Parallel,
			 .Evaluate = [](EvalContext &context) {
				 ++Evaluations();
				 double total = 0.0;
				 for (const Value &value : context.Inputs("values")) {
					 total += value.AsNumber();
				 }
				 context.SetOutput("sum", Value(total));
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.square",
			 .Inputs = {{.Name = "in", .Type = "number"}},
			 .Outputs = {{.Name = "out", .Type = "number"}},
			 .Mode = ExecMode::Parallel,
			 .Evaluate = [](EvalContext &context) {
				 ++Evaluations();
				 const double value = context.Input("in").AsNumber();
				 context.SetOutput("out", Value(value * value));
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.counter",
			 .Inputs = {},
			 .Outputs = {{.Name = "count", .Type = "int"}},
			 .Pure = Purity::Impure,
			 .Evaluate = [](EvalContext &context) {
				 context.SetOutput("count", Value(int64_t{++Evaluations()}));
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.image",
			 .Outputs = {{.Name = "image", .Type = "image"}},
			 .Params = {{.Name = "bytes", .Type = "int", .Default = Value(1024)}},
			 .Evaluate = [](EvalContext &context) {
				 ++Evaluations();
				 auto payload = std::make_shared<BlobPayload>();
				 const auto size = static_cast<size_t>(context.Param("bytes").AsNumber());
				 payload->Bytes.resize(size);
				 context.SetOutput(
					 "image", Value::FromBlob("image", {.Data = payload, .Bytes = size, .Identity = size})
				 );
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.slow",
			 .Inputs = {{.Name = "in", .Type = "number", .Default = Value(1.0)}},
			 .Outputs = {{.Name = "out", .Type = "number"}},
			 .Mode = ExecMode::Async,
			 .Evaluate = [](EvalContext &context) {
				 ++Evaluations();
				 if (context.Cancelled()) {
					 return NodeResult::Failure("cancelled");
				 }
				 context.SetOutput("out", Value(context.Input("in").AsNumber() * 10.0));
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.fail",
			 .Outputs = {{.Name = "out", .Type = "number"}},
			 .Evaluate = [](EvalContext &) { return NodeResult::Failure("always fails"); }}
		);
		registry.DefineNode(
			{.Id = "test.pass",
			 .Inputs = {{.Name = "in", .Generic = "T", .Constraints = {"number", "string"}}},
			 .Outputs = {{.Name = "out", .Generic = "T"}},
			 .Evaluate = [](EvalContext &context) {
				 context.SetOutput("out", context.Input("in"));
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.text",
			 .Outputs = {{.Name = "text", .Type = "string"}},
			 .Evaluate = [](EvalContext &context) {
				 context.SetOutput("text", Value("hello"));
				 return NodeResult::Success();
			 }}
		);
		registry.DefineNode(
			{.Id = "test.int",
			 .Outputs = {{.Name = "value", .Type = "int"}},
			 .Evaluate = [](EvalContext &context) {
				 context.SetOutput("value", Value(int64_t{7}));
				 return NodeResult::Success();
			 }}
		);
		return registry;
	}

	inline NodeInstance Node(std::string id, std::string def, std::map<std::string, Value> params = {}) {
		return {.Id = std::move(id), .Def = std::move(def), .Params = std::move(params)};
	}

	inline void Add(Graph &graph, NodeInstance node) {
		const EditResult result = graph.Apply(edit::AddNode{std::move(node)});
		REQUIRE(result.Ok);
	}

	inline void
	Wire(Graph &graph, std::string from, std::string fromPort, std::string to, std::string toPort) {
		const EditResult result = graph.Apply(
			edit::AddLink{{std::move(from), std::move(fromPort), std::move(to), std::move(toPort)}}
		);
		REQUIRE(result.Ok);
	}

	inline Plan MustCompile(const Graph &graph, const Registry &registry, Domain domain = Domain::Tool) {
		Plan plan;
		const CompileResult result = Compile(graph, registry, {.Runs = domain}, plan);
		if (!result.Ok) {
			for (const auto &diagnostic : result.Diagnostics) {
				UNSCOPED_INFO(diagnostic.Node << "." << diagnostic.Port << ": " << diagnostic.Message);
			}
		}
		REQUIRE(result.Ok);
		return plan;
	}
}
