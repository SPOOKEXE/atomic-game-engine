// A bake graph as edits: recorded, written and read back.
//
// **Replaying one is `bake`'s and is tested there.** This module is the format
// and the vocabulary, and it links no importer — which is the whole reason it
// exists, so a suite that reached for `Graph` here would be a suite proving the
// split had not happened.
//
// The property the format work hangs on is that `Read(Write(d))` is `d`, so it
// is asserted against every operation kind rather than a representative one —
// and, since v0.15, against the named set a world document carries, where the
// second property is that the bytes do not depend on the order an editor
// happened to add things in.

#include <engine/bakegraph/Document.hpp>
#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.bakegraph.document")

using engine::bake::Document;
using engine::bake::DocumentStatus;
using engine::bake::NodeKind;
using engine::bake::Operation;
using engine::bake::OperationKind;
using engine::bake::PipelineSet;
using engine::bake::Read;
using engine::bake::Write;
using engine::core::Name;
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

TEST_CASE("a document records edits in order", "[bakegraph]") {
	const Document document = Simple();

	REQUIRE(document.Count() == 5);
	CHECK(document.Operations()[0].Kind == OperationKind::AddBuiltin);
	CHECK(document.Operations()[4].Kind == OperationKind::Connect);

	// Wires are not nodes, so positions count three.
	CHECK(document.NodeCount() == 3);
}

TEST_CASE("undo drops the last edit and nothing else", "[bakegraph]") {
	Document document = Simple();

	REQUIRE(document.Undo());
	CHECK(document.Count() == 4);
	CHECK(document.NodeCount() == 3);

	// **The whole of undo, and the reason the log is the document.** There is no
	// inverse operation to write and no state to unwind — the graph is whatever
	// replaying what is left produces.
	CHECK(document.Operations().back().Kind == OperationKind::Connect);
}

TEST_CASE("undo on an empty document is refused rather than wrapping", "[bakegraph]") {
	Document document;
	CHECK_FALSE(document.Undo());
	CHECK(document.Count() == 0);
}

TEST_CASE("recording validates nothing", "[bakegraph]") {
	// An editor lets somebody wire two nodes and then delete one, so a record
	// that refused mid-edit would be a panel nobody could use in the order
	// people work in. Being wrong is `Build`'s business.
	Document document;
	document.Record(Wire(9, 12));

	CHECK(document.Count() == 1);
}

// --- building ----------------------------------------------------------------

TEST_CASE("a document round trips through text", "[bakegraph]") {
	const Document document = Simple();

	Document reloaded;
	std::string offender;
	REQUIRE(Read(Write(document), reloaded, offender) == DocumentStatus::Ok);

	REQUIRE(reloaded.Count() == document.Count());
	for (size_t index = 0; index < document.Count(); index++) {
		INFO("operation " << index);
		CHECK(reloaded.Operations()[index].Kind == document.Operations()[index].Kind);
		CHECK(reloaded.Operations()[index].Text == document.Operations()[index].Text);
	}

	// The strong form: writing what was read gives the same text.
	CHECK(Write(reloaded) == Write(document));
}

TEST_CASE("every operation kind round trips", "[bakegraph]") {
	// **Every kind rather than a representative one**, because the failure this
	// guards is a field that is written and not parsed — which is invisible
	// until somebody saves the one pipeline that uses it.
	Document document;
	Operation source;
	source.Kind = OperationKind::AddSource;
	source.Text = "fox.glb";
	document.Record(std::move(source));
	document.Record(Builtin("engine.Cube"));
	document.Record(Bare(NodeKind::Import));
	document.Record(Bare(NodeKind::Smooth));
	document.Record(Bare(NodeKind::Opaque));
	document.Record(Bare(NodeKind::Mipmap));
	document.Record(Fit(2.5f));

	Operation scale;
	scale.Kind = OperationKind::AddScale;
	scale.Amount = Vector3{1.0f / 3.0f, 2.0f, -0.5f};
	document.Record(std::move(scale));

	Operation resize;
	resize.Kind = OperationKind::AddResize;
	resize.Width = 256;
	resize.Height = 128;
	document.Record(std::move(resize));

	// **Two integers like a resize and a separate kind all the same**, because
	// they are different instructions: one resamples a picture and the other
	// says how big a drawing is first drawn.
	Operation rasterize;
	rasterize.Kind = OperationKind::AddRasterize;
	rasterize.Width = 64;
	rasterize.Height = 32;
	document.Record(std::move(rasterize));

	Operation retime;
	retime.Kind = OperationKind::AddRetime;
	retime.Number = 12.5f;
	document.Record(std::move(retime));

	document.Record(WriteNode("fox.amesh"));
	document.Record(Wire(2, 6));

	Document reloaded;
	std::string offender;
	REQUIRE(Read(Write(document), reloaded, offender) == DocumentStatus::Ok);
	REQUIRE(reloaded.Count() == document.Count());
	CHECK(Write(reloaded) == Write(document));

	// The awkward float specifically: a third is not representable, so a format
	// that wrote a rounded decimal would reload a different bake.
	CHECK(reloaded.Operations()[7].Amount.X == document.Operations()[7].Amount.X);
	CHECK(reloaded.Operations()[10].Number == 12.5f);
	CHECK(reloaded.Operations()[8].Width == 256);
	CHECK(reloaded.Operations()[9].Kind == OperationKind::AddRasterize);
	CHECK(reloaded.Operations()[9].Height == 32);
	CHECK(reloaded.Operations()[12].To == 6);
}

TEST_CASE("a name holding a newline cannot forge an operation", "[bakegraph]") {
	// The attack this escaping exists for: an asset named with a newline and a
	// `write` after it would otherwise add a node nobody asked for.
	Document document;
	document.Record(Builtin("cube\nwrite \"stolen.amesh\""));

	Document reloaded;
	std::string offender;
	REQUIRE(Read(Write(document), reloaded, offender) == DocumentStatus::Ok);

	REQUIRE(reloaded.Count() == 1);
	CHECK(reloaded.Operations()[0].Text == "cube\nwrite \"stolen.amesh\"");
}

TEST_CASE("names holding quotes, tabs and backslashes round trip", "[bakegraph]") {
	Document document;
	document.Record(WriteNode("a\\b\"c\td"));

	Document reloaded;
	std::string offender;
	REQUIRE(Read(Write(document), reloaded, offender) == DocumentStatus::Ok);
	CHECK(reloaded.Operations()[0].Text == "a\\b\"c\td");
}

TEST_CASE("text with no header is refused", "[bakegraph]") {
	Document document;
	std::string offender;

	CHECK(Read("builtin \"cube\"\n", document, offender) == DocumentStatus::Malformed);
	CHECK(Read("", document, offender) == DocumentStatus::Malformed);
	CHECK(Read("bakegraph 2\n", document, offender) == DocumentStatus::Malformed);
}

TEST_CASE("an unknown operation is refused and quotes the line", "[bakegraph]") {
	Document document;
	std::string offender;

	CHECK(Read("bakegraph 1\nexplode 3\n", document, offender) == DocumentStatus::Malformed);
	CHECK(offender == "explode 3");
}

TEST_CASE("a malformed number is refused rather than defaulted", "[bakegraph]") {
	Document document;
	std::string offender;

	CHECK(Read("bakegraph 1\nfit later\n", document, offender) == DocumentStatus::Malformed);
	CHECK(Read("bakegraph 1\nresize 256\n", document, offender) == DocumentStatus::Malformed);
	CHECK(Read("bakegraph 1\nconnect 1\n", document, offender) == DocumentStatus::Malformed);
	CHECK(Read("bakegraph 1\nscale 1 2\n", document, offender) == DocumentStatus::Malformed);
}

TEST_CASE("trailing text on a line is refused", "[bakegraph]") {
	// Accepting it would bake something other than what was written.
	Document document;
	std::string offender;

	CHECK(Read("bakegraph 1\nresize 256 128 64\n", document, offender) == DocumentStatus::Malformed);
	CHECK(offender == "resize 256 128 64");
}

TEST_CASE("an unterminated quote is refused", "[bakegraph]") {
	Document document;
	std::string offender;

	CHECK(Read("bakegraph 1\nbuiltin \"cube\n", document, offender) == DocumentStatus::Malformed);
}

TEST_CASE("an escape nothing writes is refused", "[bakegraph]") {
	// One spelling per name, so `Read(Write(d))` cannot be ambiguous.
	Document document;
	std::string offender;

	CHECK(Read("bakegraph 1\nbuiltin \"cu\\qbe\"\n", document, offender) == DocumentStatus::Malformed);
}

TEST_CASE("reading clears whatever the document held", "[bakegraph]") {
	Document document = Simple();
	std::string offender;

	REQUIRE(Read("bakegraph 1\nbuiltin \"cube\"\n", document, offender) == DocumentStatus::Ok);
	CHECK(document.Count() == 1);
}

TEST_CASE("a refused read leaves nothing half-parsed", "[bakegraph]") {
	Document document;
	std::string offender;

	CHECK(Read("bakegraph 1\nbuiltin \"cube\"\nexplode\n", document, offender) == DocumentStatus::Malformed);

	// One good line was read before the bad one. A caller that ignored the
	// status would otherwise build half a pipeline and publish it.
	CHECK(document.Count() == 1);
}

TEST_CASE("an empty document round trips to a header and nothing else", "[bakegraph]") {
	const Document document;
	CHECK(Write(document) == "bakegraph 1\n");

	Document reloaded;
	std::string offender;
	CHECK(Read(Write(document), reloaded, offender) == DocumentStatus::Ok);
	CHECK(reloaded.Count() == 0);
}

TEST_CASE("blank lines are skipped rather than refused", "[bakegraph]") {
	Document document;
	std::string offender;

	REQUIRE(Read("bakegraph 1\n\nbuiltin \"cube\"\n\n", document, offender) == DocumentStatus::Ok);
	CHECK(document.Count() == 1);
}

TEST_CASE("every status and operation kind has a description", "[bakegraph]") {
	for (const DocumentStatus status :
		 {DocumentStatus::Ok,
		  DocumentStatus::UnknownNode,
		  DocumentStatus::WrongNodeKind,
		  DocumentStatus::Refused,
		  DocumentStatus::Malformed,
		  DocumentStatus::TooManyOperations}) {
		CHECK(std::string(Describe(status)) != "unknown");
	}

	for (const OperationKind kind :
		 {OperationKind::AddSource,
		  OperationKind::AddBuiltin,
		  OperationKind::AddNode,
		  OperationKind::AddFit,
		  OperationKind::AddScale,
		  OperationKind::AddResize,
		  OperationKind::AddRasterize,
		  OperationKind::AddRetime,
		  OperationKind::AddWrite,
		  OperationKind::Connect}) {
		CHECK(std::string(Describe(kind)) != "unknown");
	}
}

// --- the set a world carries -------------------------------------------------

TEST_CASE("a set round trips and orders its names by text", "[bakegraph]") {
	// **Built out of order on purpose.** The order is the property: a set
	// written twice with the same contents has to produce the same bytes
	// whatever order an editor added things in, or a save file stops being
	// diffable and a round-trip test stops meaning anything.
	PipelineSet set;
	REQUIRE(set.Set(Name("zebra"), Simple()));
	REQUIRE(set.Set(Name("alpha"), Simple()));

	REQUIRE(set.Count() == 2);
	CHECK(set.Names()[0] == Name("alpha"));
	CHECK(set.Names()[1] == Name("zebra"));

	PipelineSet reloaded;
	std::string offender;
	REQUIRE(Read(Write(set), reloaded, offender) == DocumentStatus::Ok);
	REQUIRE(reloaded.Count() == 2);
	CHECK(Write(reloaded) == Write(set));

	const Document *alpha = reloaded.Find(Name("alpha"));
	REQUIRE(alpha != nullptr);
	CHECK(Write(*alpha) == Write(Simple()));

	// **The one-document format is a strict substring of this one**, which is
	// what "one grammar for a pipeline" means in bytes rather than in prose.
	const std::string operations = Write(Simple()).substr(std::string("bakegraph 1\n").size());
	CHECK(Write(set).find(operations) != std::string::npos);
}

TEST_CASE("setting a name twice replaces rather than appends", "[bakegraph]") {
	// Saving over a pipeline is what "save" means; a duplicate is only a
	// mistake inside one graph.
	PipelineSet set;
	REQUIRE(set.Set(Name("main"), Simple()));
	REQUIRE(set.Set(Name("main"), Document()));

	REQUIRE(set.Count() == 1);
	REQUIRE(set.Find(Name("main")) != nullptr);
	CHECK(set.Find(Name("main"))->Count() == 0);

	CHECK(set.Remove(Name("main")));
	CHECK_FALSE(set.Remove(Name("main")));
	CHECK(set.Find(Name("main")) == nullptr);
	CHECK(set.Count() == 0);
}

TEST_CASE("an unnamed pipeline is refused, in memory and in text", "[bakegraph]") {
	// A pipeline this cannot name is a pipeline it cannot write back.
	PipelineSet set;
	CHECK_FALSE(set.Set(Name(""), Simple()));
	CHECK(set.Count() == 0);

	std::string offender;
	CHECK(Read("bakegraph 1\npipeline \"\"\n", set, offender) == DocumentStatus::Malformed);
	CHECK(offender == "pipeline \"\"");
}

TEST_CASE("a pipeline naming a node kind this build lacks is refused", "[bakegraph]") {
	// **The load-time validation the split was taken to keep.** A newer editor's
	// `bevel` is indistinguishable from a typo — the vocabulary is a closed list
	// — so the text is refused here and the caller decides what that costs. For
	// a world document it costs the pipelines and not the world; see
	// `game::ReadAssetPipelines`.
	PipelineSet set;
	std::string offender;

	CHECK(
		Read("bakegraph 1\npipeline \"main\"\nbuiltin \"engine.Cube\"\nnode bevel\n", set, offender) ==
		DocumentStatus::Malformed
	);
	CHECK(offender == "node bevel");
}

TEST_CASE("operations before the first pipeline line are refused", "[bakegraph]") {
	// A set is not a document with extra parts. An operation belonging to no
	// named pipeline is a file somebody hand-edited into a shape this cannot
	// represent, and accepting it would silently drop those operations on the
	// next save.
	PipelineSet set;
	std::string offender;

	CHECK(Read("bakegraph 1\nbuiltin \"engine.Cube\"\n", set, offender) == DocumentStatus::Malformed);
	CHECK(offender == "builtin \"engine.Cube\"");

	CHECK(Read("pipeline \"main\"\n", set, offender) == DocumentStatus::Malformed);
	CHECK(Read("bakegraph 1\n", set, offender) == DocumentStatus::Ok);
	CHECK(set.Count() == 0);
}

TEST_CASE("an empty set writes a header and nothing else", "[bakegraph]") {
	// **What a world with no pipelines must cost, and it is zero.** `game` skips
	// the element entirely for an empty set, so this is only the format's half
	// of that promise.
	const PipelineSet set;
	CHECK(Write(set) == "bakegraph 1\n");
}
