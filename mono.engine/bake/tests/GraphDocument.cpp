// Replaying a document into a graph.
//
// **All of it headless, which is the point of the design.** `Build` takes a
// `SourceResolver`, so the one place a filesystem would enter is a callback —
// and every case below hands it a map. `bake` touches no disk and this file
// does not make it start.
//
// The format itself — recording, writing and reading back — is
// `Engine::bakegraph`'s and is tested there. What is here is the half that
// needs an importer.

#include <engine/bake/GraphDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.bake.graphdocument")

using engine::bake::Document;
using engine::bake::DocumentStatus;
using engine::bake::Graph;
using engine::bake::NodeKind;
using engine::bake::Operation;
using engine::bake::OperationKind;
using engine::bake::Read;
using engine::bake::SourceResolver;
using engine::bake::Write;
using engine::core::Vector3;

namespace {
	Operation Builtin(std::string name) {
		Operation operation;
		operation.Kind = OperationKind::AddBuiltin;
		operation.Text = std::move(name);
		return operation;
	}

	Operation Bare(NodeKind kind) {
		Operation operation;
		operation.Kind = OperationKind::AddNode;
		operation.Node = kind;
		return operation;
	}

	Operation Fit(float size) {
		Operation operation;
		operation.Kind = OperationKind::AddFit;
		operation.Number = size;
		return operation;
	}

	Operation WriteNode(std::string name) {
		Operation operation;
		operation.Kind = OperationKind::AddWrite;
		operation.Text = std::move(name);
		return operation;
	}

	Operation Wire(uint32_t from, uint32_t to) {
		Operation operation;
		operation.Kind = OperationKind::Connect;
		operation.From = from;
		operation.To = to;
		return operation;
	}

	// A resolver over a map, which is what keeps every case here off the disk.
	SourceResolver ResolverOver(const std::map<std::string, std::vector<std::byte>> &files) {
		return [&files](std::string_view name) -> std::span<const std::byte> {
			const auto found = files.find(std::string(name));
			if (found == files.end()) {
				return {};
			}
			return found->second;
		};
	}

	// The smallest document that builds: a built-in mesh, fitted, written.
	Document Simple() {
		Document document;
		document.Record(Builtin("engine.Cube"));
		document.Record(Fit(2.0f));
		document.Record(WriteNode("box.amesh"));
		document.Record(Wire(1, 2));
		document.Record(Wire(2, 3));
		return document;
	}
}

// --- recording ---------------------------------------------------------------

TEST_CASE("a document builds the graph it describes", "[bake]") {
	const Document document = Simple();
	const std::map<std::string, std::vector<std::byte>> files;

	Graph graph;
	std::string offender;
	REQUIRE(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::Ok);
	CHECK(offender.empty());

	// It runs, which is the only claim worth making about a built graph — that
	// it is a pipeline rather than a set of disconnected nodes.
	std::string failure;
	REQUIRE(graph.Run(failure));
	CHECK(failure.empty());
}

TEST_CASE("a wire naming a node the document does not hold is refused", "[bake]") {
	Document document;
	document.Record(Builtin("engine.Cube"));
	document.Record(Wire(1, 7));

	Graph graph;
	std::string offender;
	const std::map<std::string, std::vector<std::byte>> files;
	CHECK(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::UnknownNode);

	// The operation's position, so an editor can highlight the row.
	CHECK(offender == "2 (connect)");
}

TEST_CASE("a wire naming a node declared later is refused", "[bake]") {
	// **Positions are resolved as the replay reaches them, not up front.** A
	// forward wire would be a document whose meaning depended on a second pass,
	// and `Graph::Connect` refuses cycles on the same principle.
	Document document;
	document.Record(Builtin("engine.Cube"));
	document.Record(Wire(1, 2));
	document.Record(WriteNode("box.amesh"));

	Graph graph;
	std::string offender;
	const std::map<std::string, std::vector<std::byte>> files;
	CHECK(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::UnknownNode);
}

TEST_CASE("a parameterised kind sent as a bare node is refused", "[bake]") {
	// `Fit` carries a size and `AddNode` has nowhere to put it, so accepting
	// this would silently build a fit-to-zero.
	Document document;
	document.Record(Bare(NodeKind::Fit));

	Graph graph;
	std::string offender;
	const std::map<std::string, std::vector<std::byte>> files;
	CHECK(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::WrongNodeKind);
	CHECK(offender == "1 (node)");
}

TEST_CASE("an input kind sent as a bare node is refused", "[bake]") {
	// Same argument: `Source` needs bytes and `Builtin` needs a name.
	Document document;
	document.Record(Bare(NodeKind::Source));

	Graph graph;
	std::string offender;
	const std::map<std::string, std::vector<std::byte>> files;
	CHECK(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::WrongNodeKind);
}

TEST_CASE("an unknown built-in is refused and names itself", "[bake]") {
	Document document;
	document.Record(Builtin("not.a.builtin"));

	Graph graph;
	std::string offender;
	const std::map<std::string, std::vector<std::byte>> files;
	CHECK(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::Refused);
	CHECK(offender == "not.a.builtin");
}

TEST_CASE("a source the resolver has never heard of is refused", "[bake]") {
	Document document;
	Operation source;
	source.Kind = OperationKind::AddSource;
	source.Text = "missing.glb";
	document.Record(std::move(source));

	Graph graph;
	std::string offender;
	const std::map<std::string, std::vector<std::byte>> files;
	CHECK(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::Refused);
	CHECK(offender == "missing.glb");
}

TEST_CASE("a source is resolved to bytes at build time", "[bake]") {
	// **The document holds a name and the bytes arrive from outside**, which is
	// what keeps a saved pipeline small and what keeps a filesystem out of L9.
	std::map<std::string, std::vector<std::byte>> files;
	files["fox.glb"] = std::vector<std::byte>(16, std::byte{7});

	Document document;
	Operation source;
	source.Kind = OperationKind::AddSource;
	source.Text = "fox.glb";
	document.Record(std::move(source));

	Graph graph;
	std::string offender;
	CHECK(Build(document, graph, ResolverOver(files), offender) == DocumentStatus::Ok);
}

TEST_CASE("a null resolver is the same event as an unknown name", "[bake]") {
	Document document;
	Operation source;
	source.Kind = OperationKind::AddSource;
	source.Text = "fox.glb";
	document.Record(std::move(source));

	Graph graph;
	std::string offender;
	CHECK(Build(document, graph, nullptr, offender) == DocumentStatus::Refused);
	CHECK(offender == "fox.glb");
}

TEST_CASE("a document with no source builds without a resolver", "[bake]") {
	// The ordinary case for a pipeline over built-ins, and a caller should not
	// have to invent a callback to say it has no files.
	Graph graph;
	std::string offender;
	CHECK(Build(Simple(), graph, nullptr, offender) == DocumentStatus::Ok);
}

// --- the text format ---------------------------------------------------------
