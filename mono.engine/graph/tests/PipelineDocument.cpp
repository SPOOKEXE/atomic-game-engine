// A render graph as edits: recorded, replayed, written, read back.
//
// **The headline case is that the standard frame survives the round trip.**
// `StandardDocument` builds a graph that compiles to the same three blocks
// `StandardGraph` does, so the document layer is checked against the real frame
// and not only against fixtures — which is the difference between a format that
// works and one that works on the examples somebody wrote for it.

#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::CompiledGraph;
using engine::graph::Edit;
using engine::graph::EditKind;
using engine::graph::GraphStatus;
using engine::graph::NodeScope;
using engine::graph::PipelineDocument;
using engine::graph::PipelineDocumentStatus;
using engine::graph::PipelineSet;
using engine::graph::Read;
using engine::graph::RenderGraph;
using engine::graph::ResourceKind;
using engine::graph::StandardDocument;
using engine::graph::StandardGraph;
using engine::graph::Write;

namespace {
	Edit Resource(std::string_view name, ResourceKind kind = ResourceKind::Colour) {
		Edit edit;
		edit.Kind = EditKind::AddResource;
		edit.Name = Name(name);
		edit.Resource = kind;
		return edit;
	}

	Edit NodeEdit(std::string_view name, bool perView = true) {
		Edit edit;
		edit.Kind = EditKind::AddNode;
		edit.Name = Name(name);
		edit.NodeKind = Name(name);
		edit.Scope = perView ? NodeScope::View : NodeScope::Frame;
		return edit;
	}

	Edit Touch(EditKind kind, std::string_view target) {
		Edit edit;
		edit.Kind = kind;
		edit.Target = Name(target);
		return edit;
	}

	Edit Enable(std::string_view name, bool enabled) {
		Edit edit;
		edit.Kind = EditKind::Enable;
		edit.Name = Name(name);
		edit.Enabled = enabled;
		return edit;
	}
}

// --- the standard frame -------------------------------------------------------

TEST_CASE("the standard document builds the standard frame", "[graph]") {
	RenderGraph graph;
	Name offender;
	REQUIRE(Build(StandardDocument(), graph, offender) == PipelineDocumentStatus::Ok);

	CompiledGraph fromDocument;
	REQUIRE(graph.Compile(fromDocument, offender) == GraphStatus::Ok);

	CompiledGraph fromCode;
	const RenderGraph standard = StandardGraph();
	REQUIRE(standard.Compile(fromCode, offender) == GraphStatus::Ok);

	// **The same three blocks, node for node.** A document that produced a
	// different frame from the code it describes would be a save file that
	// changed the picture on load.
	REQUIRE(fromDocument.Shared.size() == fromCode.Shared.size());
	REQUIRE(fromDocument.PerView.size() == fromCode.PerView.size());
	REQUIRE(fromDocument.Final.size() == fromCode.Final.size());

	for (size_t index = 0; index < fromCode.PerView.size(); index++) {
		INFO("per-view position " << index);
		CHECK(graph.Find(fromDocument.PerView[index])->Name == standard.Find(fromCode.PerView[index])->Name);
	}
	CHECK(graph.Find(fromDocument.Shared.front())->Name == Name("shadow"));
	CHECK(graph.Find(fromDocument.Final.back())->Name == Name("interface"));
}

TEST_CASE("the standard document round trips through text", "[graph]") {
	const PipelineDocument document = StandardDocument();

	PipelineDocument reloaded;
	Name offender;
	REQUIRE(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);

	REQUIRE(reloaded.Count() == document.Count());
	CHECK(Write(reloaded) == Write(document));

	// And the reloaded one still builds a frame that compiles.
	RenderGraph graph;
	CHECK(Build(reloaded, graph, offender) == PipelineDocumentStatus::Ok);
}

// --- recording ----------------------------------------------------------------

TEST_CASE("undo drops the last edit", "[graph]") {
	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(NodeEdit("opaque"));

	REQUIRE(document.Count() == 2);
	REQUIRE(document.Undo());
	CHECK(document.Count() == 1);

	CHECK_FALSE(PipelineDocument{}.Undo());
}

TEST_CASE("recording validates nothing", "[graph]") {
	// An editor lets somebody declare a read and then rename the resource, so
	// being wrong is `Build`'s business rather than `Record`'s.
	PipelineDocument document;
	document.Record(Touch(EditKind::Reads, "never.declared"));
	CHECK(document.Count() == 1);
}

// --- building -----------------------------------------------------------------

TEST_CASE("a read naming an undeclared resource is refused", "[graph]") {
	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(NodeEdit("opaque"));
	document.Record(Touch(EditKind::Writes, "colour"));
	document.Record(Touch(EditKind::Reads, "shadow"));

	RenderGraph graph;
	Name offender;
	CHECK(Build(document, graph, offender) == PipelineDocumentStatus::UnknownName);
	CHECK(offender == Name("shadow"));
}

TEST_CASE("a read before any node is refused", "[graph]") {
	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(Touch(EditKind::Reads, "colour"));

	RenderGraph graph;
	Name offender;
	CHECK(Build(document, graph, offender) == PipelineDocumentStatus::UnknownName);
}

TEST_CASE("enabling a node the document has not declared is refused", "[graph]") {
	PipelineDocument document;
	document.Record(Enable("opaque", false));

	RenderGraph graph;
	Name offender;
	CHECK(Build(document, graph, offender) == PipelineDocumentStatus::UnknownName);
	CHECK(offender == Name("opaque"));
}

TEST_CASE("a duplicate resource name is refused", "[graph]") {
	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(Resource("colour", ResourceKind::Depth));

	RenderGraph graph;
	Name offender;
	CHECK(Build(document, graph, offender) == PipelineDocumentStatus::Refused);
	CHECK(offender == Name("colour"));
}

TEST_CASE("a document whose graph will not compile is refused at load", "[graph]") {
	// **A broken save file says so on load.** This one reads a resource nothing
	// wrote, which `RenderGraph::Compile` calls `ReadsBeforeWrite` — and a
	// caller that got `Ok` here would find out at the first frame instead.
	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(Resource("shadow", ResourceKind::Depth));
	document.Record(NodeEdit("opaque"));
	document.Record(Touch(EditKind::Reads, "shadow"));
	document.Record(Touch(EditKind::Writes, "colour"));

	RenderGraph graph;
	Name offender;
	CHECK(Build(document, graph, offender) == PipelineDocumentStatus::Invalid);
}

TEST_CASE("a node that writes nothing is refused at load", "[graph]") {
	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(NodeEdit("ghost"));

	RenderGraph graph;
	Name offender;
	CHECK(Build(document, graph, offender) == PipelineDocumentStatus::Invalid);
}

TEST_CASE("an enable edit survives the round trip and the build", "[graph]") {
	// The reason `SetEnabled` is an operation rather than a field: a pass
	// somebody switched off has to survive a save.
	PipelineDocument document = StandardDocument();
	document.Record(Enable("transparent", false));

	PipelineDocument reloaded;
	Name offender;
	REQUIRE(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);

	RenderGraph graph;
	REQUIRE(Build(reloaded, graph, offender) == PipelineDocumentStatus::Ok);

	CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	// Out of the compile entirely, which is what disabling means here.
	CHECK(compiled.PerView.size() == 2);
}

TEST_CASE("per-view and optional survive the round trip", "[graph]") {
	// The field the whole version turns on, and the one a text format is most
	// likely to write and forget to parse.
	PipelineDocument document;
	document.Record(Resource("shadow", ResourceKind::Depth));

	Edit shared = NodeEdit("shadow", false);
	shared.Optional = true;
	document.Record(std::move(shared));
	document.Record(Touch(EditKind::Writes, "shadow"));

	PipelineDocument reloaded;
	Name offender;
	REQUIRE(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);

	REQUIRE(reloaded.Count() == 3);
	CHECK(reloaded.Edits()[1].Scope == NodeScope::Frame);
	CHECK(reloaded.Edits()[1].Optional);
	CHECK(reloaded.Edits()[0].Resource == ResourceKind::Depth);
}

// --- the text format ----------------------------------------------------------

TEST_CASE("a name holding a newline cannot forge an edit", "[graph]") {
	PipelineDocument document;
	document.Record(Resource("colour\nnode \"stolen\" \"stolen\" yes no"));

	PipelineDocument reloaded;
	Name offender;
	REQUIRE(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);

	REQUIRE(reloaded.Count() == 1);
	CHECK(reloaded.Edits()[0].Name == Name("colour\nnode \"stolen\" \"stolen\" yes no"));
}

TEST_CASE("text with no header is refused", "[graph]") {
	PipelineDocument document;
	Name offender;

	CHECK(Read("", document, offender) == PipelineDocumentStatus::Malformed);
	CHECK(Read("renderpipeline 2\n", document, offender) == PipelineDocumentStatus::Malformed);
	CHECK(Read("node \"a\" \"a\" yes no\n", document, offender) == PipelineDocumentStatus::Malformed);
}

TEST_CASE("a malformed edit is refused rather than defaulted", "[graph]") {
	PipelineDocument document;
	Name offender;

	CHECK(Read("renderpipeline 1\nexplode\n", document, offender) == PipelineDocumentStatus::Malformed);
	CHECK(
		Read("renderpipeline 1\nresource \"a\" glitter 0 0\n", document, offender) ==
		PipelineDocumentStatus::Malformed
	);
	CHECK(
		Read("renderpipeline 1\nnode \"a\" \"a\" maybe no\n", document, offender) ==
		PipelineDocumentStatus::Malformed
	);
	CHECK(
		Read("renderpipeline 1\nresource \"a\" colour 0\n", document, offender) ==
		PipelineDocumentStatus::Malformed
	);
}

TEST_CASE("trailing text on a line is refused", "[graph]") {
	PipelineDocument document;
	Name offender;

	CHECK(
		Read("renderpipeline 1\nreads \"colour\" extra\n", document, offender) ==
		PipelineDocumentStatus::Malformed
	);
	CHECK(offender == Name("reads \"colour\" extra"));
}

TEST_CASE("an empty document round trips to a header and nothing else", "[graph]") {
	const PipelineDocument document;
	CHECK(Write(document) == "renderpipeline 1\n");

	PipelineDocument reloaded;
	Name offender;
	CHECK(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);
	CHECK(reloaded.Count() == 0);
}

TEST_CASE("every status and edit kind has a description", "[graph]") {
	for (const PipelineDocumentStatus status :
		 {PipelineDocumentStatus::Ok,
		  PipelineDocumentStatus::UnknownName,
		  PipelineDocumentStatus::Refused,
		  PipelineDocumentStatus::Malformed,
		  PipelineDocumentStatus::Invalid}) {
		CHECK(std::string(Describe(status)) != "unknown");
	}

	for (const EditKind kind :
		 {EditKind::AddResource, EditKind::AddNode, EditKind::Reads, EditKind::Writes, EditKind::Enable}) {
		CHECK(std::string(Describe(kind)) != "unknown");
	}
}

// --- many trees in one editor -------------------------------------------------

TEST_CASE("a set holds several named pipelines", "[graph]") {
	PipelineSet set;
	REQUIRE(set.Set(Name("main"), StandardDocument()));

	PipelineDocument cheap;
	cheap.Record(Resource("colour"));
	cheap.Record(NodeEdit("opaque"));
	cheap.Record(Touch(EditKind::Writes, "colour"));
	REQUIRE(set.Set(Name("reflection"), cheap));

	CHECK(set.Count() == 2);
	REQUIRE(set.Find(Name("main")) != nullptr);
	CHECK(set.Find(Name("reflection"))->Count() == 3);
	CHECK(set.Find(Name("absent")) == nullptr);
}

TEST_CASE("names come back sorted whatever order they went in", "[graph]") {
	// **What makes a save byte-identical run to run**, and therefore diffable.
	PipelineSet set;
	set.Set(Name("zebra"), PipelineDocument{});
	set.Set(Name("alpha"), PipelineDocument{});
	set.Set(Name("middle"), PipelineDocument{});

	REQUIRE(set.Count() == 3);
	CHECK(set.Names()[0] == Name("alpha"));
	CHECK(set.Names()[1] == Name("middle"));
	CHECK(set.Names()[2] == Name("zebra"));

	PipelineSet other;
	other.Set(Name("middle"), PipelineDocument{});
	other.Set(Name("zebra"), PipelineDocument{});
	other.Set(Name("alpha"), PipelineDocument{});
	CHECK(Write(other) == Write(set));
}

TEST_CASE("setting an existing name replaces rather than refuses", "[graph]") {
	// Saving over a pipeline is what "save" means.
	PipelineSet set;
	set.Set(Name("main"), PipelineDocument{});
	REQUIRE(set.Find(Name("main"))->Count() == 0);

	set.Set(Name("main"), StandardDocument());
	CHECK(set.Count() == 1);
	CHECK(set.Find(Name("main"))->Count() > 0);
}

TEST_CASE("an unnamed pipeline is refused", "[graph]") {
	PipelineSet set;
	CHECK_FALSE(set.Set(Name{}, StandardDocument()));
	CHECK(set.Count() == 0);
}

TEST_CASE("removing takes the document with the name", "[graph]") {
	PipelineSet set;
	set.Set(Name("main"), StandardDocument());
	set.Set(Name("debug"), PipelineDocument{});

	REQUIRE(set.Remove(Name("main")));
	CHECK(set.Count() == 1);
	CHECK(set.Find(Name("main")) == nullptr);
	CHECK(set.Names()[0] == Name("debug"));

	CHECK_FALSE(set.Remove(Name("main")));
}

TEST_CASE("a set round trips through text", "[graph]") {
	PipelineSet set;
	set.Set(Name("main"), StandardDocument());

	PipelineDocument cheap;
	cheap.Record(Resource("colour"));
	cheap.Record(NodeEdit("opaque", false));
	cheap.Record(Touch(EditKind::Writes, "colour"));
	set.Set(Name("reflection"), cheap);

	PipelineSet reloaded;
	Name offender;
	REQUIRE(Read(Write(set), reloaded, offender) == PipelineDocumentStatus::Ok);

	REQUIRE(reloaded.Count() == 2);
	CHECK(Write(reloaded) == Write(set));

	// The per-pipeline contents survived, including the field the version turns
	// on.
	REQUIRE(reloaded.Find(Name("reflection")) != nullptr);
	CHECK(reloaded.Find(Name("reflection"))->Edits()[1].Scope == NodeScope::Frame);

	// And the main one still builds a frame that compiles.
	RenderGraph graph;
	CHECK(Build(*reloaded.Find(Name("main")), graph, offender) == PipelineDocumentStatus::Ok);
}

TEST_CASE("an empty set round trips to a header and nothing else", "[graph]") {
	const PipelineSet set;
	CHECK(Write(set) == "renderpipelines 1\n");

	PipelineSet reloaded;
	Name offender;
	CHECK(Read(Write(set), reloaded, offender) == PipelineDocumentStatus::Ok);
	CHECK(reloaded.Count() == 0);
}

TEST_CASE("a set and a document do not read as each other", "[graph]") {
	// **Different first words**, so a reader knows which shape it is holding
	// from the first line rather than from whether a `pipeline` line turns up.
	PipelineSet set;
	PipelineDocument document;
	Name offender;

	CHECK(Read(Write(StandardDocument()), set, offender) == PipelineDocumentStatus::Malformed);
	CHECK(Read(Write(PipelineSet{}), document, offender) == PipelineDocumentStatus::Malformed);
}

TEST_CASE("an edit before the first pipeline line is refused", "[graph]") {
	PipelineSet set;
	Name offender;

	CHECK(
		Read("renderpipelines 1\nresource \"colour\" colour 0 0\n", set, offender) ==
		PipelineDocumentStatus::Malformed
	);
	CHECK(offender == Name("resource \"colour\" colour 0 0"));
}

TEST_CASE("a malformed edit inside a named pipeline names the line", "[graph]") {
	PipelineSet set;
	Name offender;

	CHECK(
		Read("renderpipelines 1\npipeline \"main\"\nexplode\n", set, offender) ==
		PipelineDocumentStatus::Malformed
	);
	CHECK(offender == Name("explode"));
}

TEST_CASE("a pipeline line with no name is refused", "[graph]") {
	PipelineSet set;
	Name offender;

	CHECK(Read("renderpipelines 1\npipeline\n", set, offender) == PipelineDocumentStatus::Malformed);
	CHECK(Read("renderpipelines 1\npipeline \"\"\n", set, offender) == PipelineDocumentStatus::Malformed);
}

TEST_CASE("a pipeline name holding a newline cannot forge a second pipeline", "[graph]") {
	PipelineSet set;
	set.Set(Name("main\npipeline \"stolen\""), StandardDocument());

	PipelineSet reloaded;
	Name offender;
	REQUIRE(Read(Write(set), reloaded, offender) == PipelineDocumentStatus::Ok);
	CHECK(reloaded.Count() == 1);
}
