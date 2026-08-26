// A render graph as edits: recorded, replayed, written, read back.
//
// **The headline case is that the engine's default frame survives the round
// trip.** The document layer is checked against the graph the renderer installs,
// not only against fixtures.

#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::CompiledGraph;
using engine::graph::DefaultPbrDocument;
using engine::graph::Edit;
using engine::graph::EditKind;
using engine::graph::GraphStatus;
using engine::graph::NodeId;
using engine::graph::NodeScope;
using engine::graph::PipelineDocument;
using engine::graph::PipelineDocumentStatus;
using engine::graph::PipelineSet;
using engine::graph::Read;
using engine::graph::RenderGraph;
using engine::graph::ResourceKind;
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

	Edit NodeKindEdit(std::string_view name, std::string_view kind) {
		Edit edit = NodeEdit(name);
		edit.NodeKind = Name(kind);
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

	Edit Parameter(std::string_view key, std::string value) {
		Edit edit;
		edit.Kind = EditKind::Set;
		edit.Key = Name(key);
		edit.Value = std::move(value);
		return edit;
	}
}

// --- the default frame --------------------------------------------------------

TEST_CASE("the default document builds the engine frame", "[graph]") {
	RenderGraph graph;
	Name offender;
	REQUIRE(Build(DefaultPbrDocument(), graph, offender) == PipelineDocumentStatus::Ok);

	CompiledGraph fromDocument;
	REQUIRE(graph.Compile(fromDocument, offender) == GraphStatus::Ok);

	REQUIRE(fromDocument.Shared.size() == 2);
	REQUIRE(fromDocument.PerView.size() == 18);
	REQUIRE(fromDocument.Final.size() == 4);
	CHECK(graph.Find(fromDocument.Shared.front())->Name == Name("world"));
	CHECK(graph.Find(fromDocument.Shared.back())->Name == Name("shadow"));
	CHECK(graph.Find(fromDocument.PerView.front())->Name == Name("camera"));
	CHECK(graph.Find(fromDocument.PerView.back())->Name == Name("transparent"));
	CHECK(graph.Find(fromDocument.Final.back())->Name == Name("output-image"));
}

TEST_CASE("the default document round trips through text", "[graph]") {
	const PipelineDocument document = DefaultPbrDocument();

	PipelineDocument reloaded;
	Name offender;
	REQUIRE(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);

	REQUIRE(reloaded.Count() == document.Count());
	CHECK(Write(reloaded) == Write(document));

	// And the reloaded one still builds a frame that compiles.
	RenderGraph graph;
	CHECK(Build(reloaded, graph, offender) == PipelineDocumentStatus::Ok);
}

TEST_CASE("version one documents upgrade transient resources to version two", "[graph][document]") {
	PipelineDocument document;
	Name offender;
	REQUIRE(
		Read("renderpipeline 1\nresource \"colour\" colour RGBA8 0 0 1\n", document, offender) ==
		PipelineDocumentStatus::Ok
	);
	REQUIRE(document.Count() == 1);
	CHECK_FALSE(document.Edits()[0].External);
	CHECK(Write(document) == "renderpipeline 2\nresource \"colour\" colour RGBA8 0 0 1 no\n");
}

TEST_CASE("the default PBR document carries material emission and ambient occlusion", "[graph]") {
	RenderGraph graph;
	Name offender;
	REQUIRE(Build(DefaultPbrDocument(), graph, offender) == PipelineDocumentStatus::Ok);

	CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
	REQUIRE(compiled.Shared.size() == 2);
	REQUIRE(compiled.PerView.size() == 18);
	REQUIRE(compiled.Final.size() == 4);

	CHECK(graph.Find(compiled.Shared[0])->Kind == Name("world"));
	CHECK(graph.Find(compiled.Shared[1])->Kind == Name("shadow"));
	CHECK(graph.Find(compiled.PerView[0])->Kind == Name("camera"));
	CHECK(graph.Find(compiled.PerView[1])->Kind == Name("last-frame"));
	CHECK(graph.Find(compiled.PerView[2])->Kind == Name("entities"));
	CHECK(graph.Find(compiled.PerView[3])->Kind == Name("cull-frustum"));
	CHECK(graph.Find(compiled.PerView[4])->Kind == Name("order-draw"));
	CHECK(graph.Find(compiled.PerView[5])->Kind == Name("upload-instances"));
	CHECK(graph.Find(compiled.PerView[6])->Kind == Name("mirror-capture"));
	CHECK(graph.Find(compiled.PerView[7])->Kind == Name("portal-capture"));
	CHECK(graph.Find(compiled.PerView[8])->Kind == Name("portal-tonemap"));
	CHECK(graph.Find(compiled.PerView[9])->Kind == Name("gbuffer"));
	CHECK(graph.Find(compiled.PerView[11])->Kind == Name("ssao"));
	CHECK(graph.Find(compiled.PerView[12])->Kind == Name("deferred-lighting"));
	CHECK(graph.Find(compiled.PerView[13])->Kind == Name("sky"));
	CHECK(graph.Find(compiled.PerView[14])->Kind == Name("tonemap"));
	CHECK(graph.Find(compiled.PerView[15])->Kind == Name("portal-overlay"));
	CHECK(graph.Find(compiled.PerView[16])->Kind == Name("mirror-overlay"));
	CHECK(graph.Find(compiled.PerView[17])->Kind == Name("transparent"));
	CHECK(graph.Find(compiled.Final[0])->Kind == Name("present"));
	CHECK(graph.Find(compiled.Final[3])->Kind == Name("output-image"));

	bool emissive = false;
	bool occlusion = false;
	bool composedImage = false;
	for (uint32_t value = 1; value <= graph.ResourceCount(); value++) {
		const auto *resource = graph.FindResource(engine::graph::ResourceId{value});
		REQUIRE(resource != nullptr);
		emissive = emissive || (resource->Name == Name("emissive") &&
								resource->Format == engine::graph::ResourceFormat::RGBA16F);
		occlusion =
			occlusion || (resource->Name == Name("occlusion") &&
						  resource->Format == engine::graph::ResourceFormat::R8 && resource->Divisor == 2);
		composedImage = composedImage || (resource->Name == Name("composed-image") && !resource->External);
	}
	CHECK(emissive);
	CHECK(occlusion);
	CHECK(composedImage);

	PipelineDocument reloaded;
	REQUIRE(Read(Write(DefaultPbrDocument()), reloaded, offender) == PipelineDocumentStatus::Ok);
	RenderGraph roundTripped;
	CHECK(Build(reloaded, roundTripped, offender) == PipelineDocumentStatus::Ok);
}

TEST_CASE("optional default graph nodes can be disabled without breaking their consumers", "[graph]") {
	PipelineDocument document = DefaultPbrDocument();
	Edit shadow;
	shadow.Kind = EditKind::Enable;
	shadow.Name = Name("shadow");
	shadow.Enabled = false;
	document.Record(shadow);
	Edit ssao = shadow;
	ssao.Name = Name("ssao");
	document.Record(ssao);
	Edit mirrorCapture = shadow;
	mirrorCapture.Name = Name("mirror-capture");
	document.Record(mirrorCapture);

	RenderGraph graph;
	Name offender;
	REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
	CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	for (const NodeId id : compiled.Shared) {
		CHECK(graph.Find(id)->Kind != Name("shadow"));
	}
	for (const NodeId id : compiled.PerView) {
		CHECK(graph.Find(id)->Kind != Name("ssao"));
		CHECK(graph.Find(id)->Kind != Name("mirror-capture"));
	}
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

TEST_CASE("resource bindings retain their named node sockets", "[graph]") {
	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(NodeEdit("opaque"));
	Edit write = Touch(EditKind::Writes, "colour");
	write.Key = Name("colour");
	document.Record(write);

	PipelineDocument reloaded;
	Name offender;
	REQUIRE(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);
	REQUIRE(reloaded.Count() == 3);
	CHECK(reloaded.Edits()[2].Key == Name("colour"));
	CHECK(Write(reloaded) == Write(document));
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
	// wrote, which `RenderGraph::Compile` calls `ReadsBeforeWrite` - and a
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
	PipelineDocument document = DefaultPbrDocument();
	document.Record(Enable("mirror-capture", false));

	PipelineDocument reloaded;
	Name offender;
	REQUIRE(Read(Write(document), reloaded, offender) == PipelineDocumentStatus::Ok);

	RenderGraph graph;
	REQUIRE(Build(reloaded, graph, offender) == PipelineDocumentStatus::Ok);

	CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	// Out of the compile entirely, which is what disabling means here.
	CHECK(compiled.PerView.size() == 17);
}

TEST_CASE("the game interface can be disabled without removing the frame output", "[graph][interface]") {
	PipelineDocument document = DefaultPbrDocument();
	document.Record(Enable("interface", false));

	RenderGraph graph;
	Name offender;
	REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);

	CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
	REQUIRE(compiled.Final.size() == 3);
	CHECK(std::none_of(compiled.Final.begin(), compiled.Final.end(), [&](NodeId node) {
		return graph.Find(node)->Kind == Name("interface");
	}));
	CHECK(std::any_of(compiled.Final.begin(), compiled.Final.end(), [&](NodeId node) {
		return graph.Find(node)->Kind == Name("output-image");
	}));
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
	CHECK(Read("renderpipeline 3\n", document, offender) == PipelineDocumentStatus::Malformed);
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
	CHECK(Write(document) == "renderpipeline 2\n");

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
	REQUIRE(set.Set(Name("main"), DefaultPbrDocument()));

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

	set.Set(Name("main"), DefaultPbrDocument());
	CHECK(set.Count() == 1);
	CHECK(set.Find(Name("main"))->Count() > 0);
}

TEST_CASE("an unnamed pipeline is refused", "[graph]") {
	PipelineSet set;
	CHECK_FALSE(set.Set(Name{}, DefaultPbrDocument()));
	CHECK(set.Count() == 0);
}

TEST_CASE("removing takes the document with the name", "[graph]") {
	PipelineSet set;
	set.Set(Name("main"), DefaultPbrDocument());
	set.Set(Name("debug"), PipelineDocument{});

	REQUIRE(set.Remove(Name("main")));
	CHECK(set.Count() == 1);
	CHECK(set.Find(Name("main")) == nullptr);
	CHECK(set.Names()[0] == Name("debug"));

	CHECK_FALSE(set.Remove(Name("main")));
}

TEST_CASE("a set round trips through text", "[graph]") {
	PipelineSet set;
	set.Set(Name("main"), DefaultPbrDocument());

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

TEST_CASE("version one pipeline sets upgrade their resource lifetime", "[graph][document]") {
	PipelineSet set;
	Name offender;
	REQUIRE(
		Read(
			"renderpipelines 1\npipeline \"main\"\nresource \"colour\" colour RGBA8 0 0 1\n", set, offender
		) == PipelineDocumentStatus::Ok
	);
	const PipelineDocument *main = set.Find(Name("main"));
	REQUIRE(main != nullptr);
	REQUIRE(main->Count() == 1);
	CHECK_FALSE(main->Edits()[0].External);
	CHECK(Write(set) == "renderpipelines 2\npipeline \"main\"\nresource \"colour\" colour RGBA8 0 0 1 no\n");
}

TEST_CASE("an empty set round trips to a header and nothing else", "[graph]") {
	const PipelineSet set;
	CHECK(Write(set) == "renderpipelines 2\n");

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

	CHECK(Read(Write(DefaultPbrDocument()), set, offender) == PipelineDocumentStatus::Malformed);
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
	set.Set(Name("main\npipeline \"stolen\""), DefaultPbrDocument());

	PipelineSet reloaded;
	Name offender;
	REQUIRE(Read(Write(set), reloaded, offender) == PipelineDocumentStatus::Ok);
	CHECK(reloaded.Count() == 1);
}

TEST_CASE("a node's parameters survive a round trip", "[graph][document]") {
	// **The difference between a kind and a node.** Two `filter-tag` nodes are
	// the same kind and filter different tags; two `raster` nodes are the same
	// kind and run different shaders. A save file that lost that would lose
	// everything about a pipeline except its shape.
	PipelineDocument document;
	document.Record(Resource("colour"));

	document.Record(NodeKindEdit("filter", "filter-tag"));
	document.Record(Parameter("mask", "0x0f"));
	document.Record(Parameter("shader", "blur.frag"));
	document.Record(Touch(EditKind::Writes, "colour"));

	const std::string text = Write(document);
	INFO(text);

	PipelineDocument read;
	Name offender;
	REQUIRE(Read(text, read, offender) == PipelineDocumentStatus::Ok);

	// **Byte-identical, not merely equivalent.** A save file that reorders or
	// respells what it read is a file that shows a diff every time it is opened.
	CHECK(Write(read) == text);
}

TEST_CASE("parameters land on the node above them", "[graph][document]") {
	PipelineDocument document;
	document.Record(Resource("a"));
	document.Record(Resource("b"));

	document.Record(NodeKindEdit("one", "clear"));
	document.Record(Parameter("mask", "1"));
	document.Record(Touch(EditKind::Writes, "a"));

	document.Record(NodeKindEdit("two", "clear"));
	document.Record(Parameter("mask", "2"));
	document.Record(Touch(EditKind::Writes, "b"));

	RenderGraph graph;
	Name offender;
	INFO("offender: " << std::string(offender.Text()));
	REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);

	const engine::graph::Node *one = graph.Find(engine::graph::NodeId{1});
	const engine::graph::Node *two = graph.Find(engine::graph::NodeId{2});
	REQUIRE(one != nullptr);
	REQUIRE(two != nullptr);

	// **Each node keeps its own**, which is the whole point: a second node of
	// the same kind must not inherit the first's configuration.
	REQUIRE(one->Parameter(Name("mask")) != nullptr);
	REQUIRE(two->Parameter(Name("mask")) != nullptr);
	CHECK(*one->Parameter(Name("mask")) == "1");
	CHECK(*two->Parameter(Name("mask")) == "2");
}

TEST_CASE("a parameter reads as a number and falls back when it cannot", "[graph]") {
	engine::graph::Node node;
	node.Parameters.push_back(engine::graph::NodeParameter{Name("radius"), "12.5"});
	node.Parameters.push_back(engine::graph::NodeParameter{Name("mask"), "0x0f"});
	node.Parameters.push_back(engine::graph::NodeParameter{Name("count"), "31"});
	node.Parameters.push_back(engine::graph::NodeParameter{Name("half"), "1."});
	node.Parameters.push_back(engine::graph::NodeParameter{Name("junk"), "wat"});

	CHECK(node.Number(Name("radius"), 0.0f) == 12.5f);
	CHECK(node.Integer(Name("mask"), 0) == 15);
	CHECK(node.Integer(Name("count"), 0) == 31);

	// **Unset takes the fallback, and so does unreadable.** A half-typed number
	// in an editor is a state somebody is passing through, not a pipeline to
	// reject - see `Node::Number`.
	CHECK(node.Number(Name("missing"), 7.0f) == 7.0f);
	CHECK(node.Number(Name("junk"), 7.0f) == 7.0f);
	CHECK(node.Integer(Name("junk"), 9) == 9);

	// **Unset and set-to-nothing are different answers**, and only the first
	// should take a default. `Parameter` is what tells them apart.
	node.Parameters.push_back(engine::graph::NodeParameter{Name("blank"), ""});
	CHECK(node.Parameter(Name("blank")) != nullptr);
	CHECK(node.Parameter(Name("missing")) == nullptr);
}

TEST_CASE("a parameter with no node before it is refused", "[graph][document]") {
	// A `set` that configures nothing is a document built wrong rather than a
	// line to skip - the same reading `reads` takes.
	PipelineDocument document;
	document.Record(Parameter("mask", "1"));

	RenderGraph graph;
	Name offender;
	CHECK(Build(document, graph, offender) == PipelineDocumentStatus::UnknownName);
}

TEST_CASE("a parameter can carry a multi-line shader", "[graph][document]") {
	// **The quoting already escapes newlines**, which is what makes a pipeline
	// document able to carry the shader as well as the shape of a frame. A
	// shader somebody is editing has no baked form and will have a different one
	// a keystroke later - keeping it in the node is what makes the save file the
	// whole pipeline rather than a reference to files beside it.
	const std::string glsl = "#version 450\n"
							 "layout(location = 0) in vec2 inUv;\n"
							 "layout(location = 0) out vec4 outColour;\n"
							 "void main() {\n"
							 "\toutColour = vec4(inUv, 0.0, 1.0);\n"
							 "}\n";

	PipelineDocument document;
	document.Record(Resource("colour"));
	document.Record(NodeKindEdit("tint", "raster"));
	document.Record(Parameter("source", glsl));
	document.Record(Touch(EditKind::Writes, "colour"));

	const std::string text = Write(document);
	INFO(text);

	// One line per edit, whatever the shader looks like - a newline that reached
	// the file raw would make the next line of GLSL read as a document word.
	CHECK(text.find("\n#version") == std::string::npos);

	PipelineDocument read;
	Name offender;
	REQUIRE(Read(text, read, offender) == PipelineDocumentStatus::Ok);
	CHECK(Write(read) == text);

	RenderGraph graph;
	REQUIRE(Build(read, graph, offender) == PipelineDocumentStatus::Ok);

	const engine::graph::Node *node = graph.Find(engine::graph::NodeId{1});
	REQUIRE(node != nullptr);
	REQUIRE(node->Parameter(Name("source")) != nullptr);

	// Byte for byte, tabs and all: a shader that came back with its whitespace
	// rearranged would compile and diff forever.
	CHECK(*node->Parameter(Name("source")) == glsl);
}
