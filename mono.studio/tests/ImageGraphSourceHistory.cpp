#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <studio/ImageGraph.hpp>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.source_history")
TEST_DEPENDS("engine.imagegraphio.pxcxappend")

namespace {
	using namespace engine::imagegraph;
	using engine::bake::PxcxArchive;
	using engine::imagegraphio::PxcxAppendOptions;
	using engine::imagegraphio::PxcxAppendResult;
	using engine::imagegraphio::PxcxImport;
	using History = studio::ImageGraphHistory;

	PxcxArchive Archive(std::string graph) {
		PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = std::move(graph);
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	PxcxArchive Destination() {
		return Archive(
			R"JSON({"attri":{"future_root":{"keep":1}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4},"future_input":{"keep":2}}],"future_node":{"keep":3}}],"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":17}]})JSON"
		);
	}
	PxcxArchive Incoming() {
		return Archive(
			R"JSON({"nodes":[{"id":"number","type":"Node_Number_Simple","x":10,"y":20,"inputs":[{"r":{"d":9}}]},{"id":"future","type":"Vendor_Future","x":30,"y":40,"inputs":[{"r":{"d":"foreign"},"future_tail":{"keep":8}}]}]})JSON"
		);
	}
	PxcxImport Import(const PxcxArchive &archive) {
		PxcxImport imported;
		std::string failure;
		REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, imported, failure));
		return imported;
	}
	History::SourceSnapshot Snapshot(const std::vector<std::byte> &bytes) {
		return std::make_shared<const std::vector<std::byte>>(bytes);
	}
	std::vector<std::byte> ArchiveBytes(const PxcxArchive &archive) {
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		return bytes;
	}
	std::string Text(const Document &document) {
		return engine::imagegraph::Write(document);
	}
}

TEST_CASE(
	"source history follows append and ordinary native edits through exact archive undo and redo",
	"[studio][source_history][append]"
) {
	const auto destination = Destination();
	const auto incoming = Incoming();
	PxcxAppendResult appended;
	Diagnostic diagnostic;
	PxcxAppendOptions options;
	options.Namespace = "imported";
	REQUIRE(engine::imagegraphio::AppendPxcxProject(destination, incoming, options, appended, diagnostic));
	const auto before = Import(destination);
	const auto afterAppend = appended.Project;
	const auto destinationBytes = Snapshot(destination.OriginalBytes);
	const auto appendedBytes = Snapshot(afterAppend.Source.OriginalBytes);
	History history;
	Document beforeAppend = before.Graph;
	beforeAppend.Nodes.front().Position = {-13, 29};
	REQUIRE(history.TryRecord(before.Graph, beforeAppend));
	Document appendedNative = afterAppend.Graph;
	appendedNative.Nodes.front().Position = beforeAppend.Nodes.front().Position;
	int admissions = 0;
	std::vector<std::pair<History::SourceSnapshot, History::SourceSnapshot>> admittedSources;
	const auto admit = [&](const Document &,
						   const Document &,
						   const History::SourceSnapshot &oldSource,
						   const History::SourceSnapshot &newSource) {
		++admissions;
		admittedSources.emplace_back(oldSource, newSource);
		return oldSource && newSource;
	};
	REQUIRE(history.TryRecord(beforeAppend, appendedNative, destinationBytes, appendedBytes, admit));
	CHECK(admissions == 1);
	CHECK(history.CurrentSourceBytes() == appendedBytes);
	Document current = appendedNative;
	current.Nodes.front().Position = {77, -31};
	REQUIRE(history.TryRecord(appendedNative, current, appendedBytes, appendedBytes, admit));
	CHECK(history.CurrentSourceBytes() == appendedBytes);

	const auto currentBytes = engine::imagegraph::Write(current);
	REQUIRE(history.Undo(current, admit));
	CHECK(current == appendedNative);
	CHECK(history.CurrentSourceBytes() == appendedBytes);
	REQUIRE(history.Redo(current, admit));
	CHECK(engine::imagegraph::Write(current) == currentBytes);
	REQUIRE(history.Undo(current, admit));
	REQUIRE(history.Undo(current, admit));
	CHECK(current == beforeAppend);
	CHECK(history.CurrentSourceBytes() == destinationBytes);
	REQUIRE(history.Undo(current, admit));
	CHECK(current == before.Graph);
	CHECK(history.CurrentSourceBytes() == destinationBytes);
	REQUIRE(history.Redo(current, admit));
	CHECK(current == beforeAppend);
	CHECK(history.CurrentSourceBytes() == destinationBytes);
	REQUIRE(history.Redo(current, admit));
	CHECK(current == appendedNative);
	CHECK(history.CurrentSourceBytes() == appendedBytes);
	REQUIRE(history.Redo(current, admit));
	CHECK(engine::imagegraph::Write(current) == currentBytes);
	CHECK(admissions == 10);
	REQUIRE(admittedSources.size() == 10);
	CHECK(admittedSources[0].first == destinationBytes);
	CHECK(admittedSources[0].second == appendedBytes);
	CHECK(admittedSources[1].first == appendedBytes);
	CHECK(admittedSources[1].second == appendedBytes);
	CHECK(admittedSources[2].first == appendedBytes);
	CHECK(admittedSources[2].second == appendedBytes);
	CHECK(admittedSources[3].first == appendedBytes);
	CHECK(admittedSources[3].second == appendedBytes);
	CHECK(admittedSources[4].first == appendedBytes);
	CHECK(admittedSources[4].second == appendedBytes);
	CHECK(admittedSources[5].first == appendedBytes);
	CHECK(admittedSources[5].second == destinationBytes);
	CHECK(admittedSources[6].first == destinationBytes);
	CHECK(admittedSources[6].second == destinationBytes);
	CHECK(admittedSources[7].first == destinationBytes);
	CHECK(admittedSources[7].second == destinationBytes);
	CHECK(admittedSources[8].first == destinationBytes);
	CHECK(admittedSources[8].second == appendedBytes);
	CHECK(admittedSources[9].first == appendedBytes);
	CHECK(admittedSources[9].second == appendedBytes);
	CHECK(*history.CurrentSourceBytes() == *appendedBytes);
}

TEST_CASE(
	"source-only edits are transitions and refused admission leaves history and source untouched",
	"[studio][source_history][admission]"
) {
	const auto base = Destination();
	const auto baseImport = Import(base);
	const auto changedSource = Archive(
		R"JSON({"attri":{"future_root":{"keep":99}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4},"future_input":{"keep":2}}],"future_node":{"keep":3}}],"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":99}]})JSON"
	);
	CHECK(Import(changedSource).Graph == baseImport.Graph);
	const auto sourceBefore = Snapshot(base.OriginalBytes);
	const auto sourceAfter = Snapshot(changedSource.OriginalBytes);
	History history;
	int callbackCount = 0;
	const auto deny = [&](const Document &,
						  const Document &,
						  const History::SourceSnapshot &,
						  const History::SourceSnapshot &) {
		++callbackCount;
		return false;
	};
	CHECK_FALSE(history.TryRecord(baseImport.Graph, baseImport.Graph, sourceBefore, sourceAfter, deny));
	CHECK(callbackCount == 1);
	CHECK_FALSE(history.CanUndo());
	CHECK_FALSE(history.CurrentSourceBytes());

	const auto allow = [](const Document &,
						  const Document &,
						  const History::SourceSnapshot &,
						  const History::SourceSnapshot &) { return true; };
	REQUIRE(history.TryRecord(baseImport.Graph, baseImport.Graph, sourceBefore, sourceAfter, allow));
	CHECK(history.CanUndo());
	CHECK(history.CurrentSourceBytes() == sourceAfter);
	Document current = baseImport.Graph;
	const auto beforeText = engine::imagegraph::Write(current);
	CHECK_FALSE(history.Undo(current, deny));
	CHECK(engine::imagegraph::Write(current) == beforeText);
	CHECK(history.CanUndo());
	CHECK_FALSE(history.CanRedo());
	CHECK(history.CurrentSourceBytes() == sourceAfter);
	REQUIRE(history.Undo(current, allow));
	CHECK(history.CurrentSourceBytes() == sourceBefore);
	REQUIRE(history.Redo(current, allow));
	CHECK(history.CurrentSourceBytes() == sourceAfter);
}

TEST_CASE(
	"source history rejects stale baselines and byte limits without disturbing a recorded transition",
	"[studio][source_history][limits]"
) {
	const auto base = Import(Destination());
	const auto sourceA = Snapshot(base.Source.OriginalBytes);
	const auto sourceB = Snapshot(ArchiveBytes(Archive(
		R"JSON({"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":5}}]}]})JSON"
	)));
	Document after = base.Graph;
	after.Nodes.front().Position = {9, 12};
	History history;
	const auto allow = [](const Document &,
						  const Document &,
						  const History::SourceSnapshot &,
						  const History::SourceSnapshot &) { return true; };
	REQUIRE(history.TryRecord(base.Graph, after, sourceA, sourceA, allow));
	const auto beforeText = engine::imagegraph::Write(after);
	CHECK_FALSE(history.TryRecord(after, base.Graph, sourceB, sourceA, allow));
	CHECK(history.CanUndo());
	CHECK_FALSE(history.CanRedo());
	CHECK(engine::imagegraph::Write(after) == beforeText);
	CHECK(history.CurrentSourceBytes() == sourceA);

	History zeroBytes(8, 1);
	CHECK_FALSE(zeroBytes.TryRecord(base.Graph, after, sourceA, sourceB, allow));
	CHECK_FALSE(zeroBytes.CanUndo());
	CHECK_FALSE(zeroBytes.CurrentSourceBytes());
}

TEST_CASE(
	"ordinary edits share one retained source allocation at the exact byte boundary",
	"[studio][source_history][budget]"
) {
	const auto base = Import(Destination());
	const auto source = Snapshot(base.Source.OriginalBytes);
	Document initial = base.Graph;
	initial.Nodes.front().Position = {10, 20};
	Document first = initial;
	first.Nodes.front().Position = {30, 40};
	Document second = first;
	second.Nodes.front().Position = {50, 60};
	Document third = second;
	third.Nodes.front().Position = {70, 80};
	const size_t textBytes =
		std::max({Text(initial).size(), Text(first).size(), Text(second).size(), Text(third).size()});
	REQUIRE(Text(initial).size() == textBytes);
	REQUIRE(Text(first).size() == textBytes);
	REQUIRE(Text(second).size() == textBytes);
	REQUIRE(Text(third).size() == textBytes);
	const size_t exactCapacity = source->capacity() + 2 * textBytes;
	History history(8, exactCapacity);
	const auto allow = [](const Document &,
						  const Document &,
						  const History::SourceSnapshot &,
						  const History::SourceSnapshot &) { return true; };
	REQUIRE(history.TryRecord(initial, first, source, source, allow));
	REQUIRE(history.TryRecord(first, second, source, source, allow));
	REQUIRE(history.TryRecord(second, third, source, source, allow));
	CHECK(history.CanUndo());
	CHECK(history.CurrentSourceBytes() == source);
	REQUIRE(history.Undo(third, allow));
	CHECK(third == second);
	REQUIRE(history.Undo(third, allow));
	CHECK(third == first);
	CHECK_FALSE(history.Undo(third, allow));
	CHECK(history.CurrentSourceBytes() == source);
}

TEST_CASE(
	"a third source epoch evicts older transitions and undo returns to the retained prior epoch",
	"[studio][source_history][epochs]"
) {
	const auto base = Import(Destination());
	const auto sourceA = Snapshot(base.Source.OriginalBytes);
	const auto sourceB = Snapshot(ArchiveBytes(Archive(
		R"JSON({"attri":{"future_root":{"keep":2}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4},"future_input":{"keep":2}}],"future_node":{"keep":3}}],"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":17}]})JSON"
	)));
	const auto sourceC = Snapshot(ArchiveBytes(Archive(
		R"JSON({"attri":{"future_root":{"keep":3}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4},"future_input":{"keep":2}}],"future_node":{"keep":3}}],"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":17}]})JSON"
	)));
	const size_t textBytes = Text(base.Graph).size();
	const size_t byteCapacity =
		std::max({sourceA->capacity() + sourceB->capacity(), sourceB->capacity() + sourceC->capacity()}) +
		2 * textBytes;
	const size_t allThreeEpochs =
		sourceA->capacity() + sourceB->capacity() + sourceC->capacity() + 2 * textBytes;
	REQUIRE(byteCapacity < allThreeEpochs);
	History history(8, byteCapacity);
	const auto allow = [](const Document &,
						  const Document &,
						  const History::SourceSnapshot &,
						  const History::SourceSnapshot &) { return true; };
	REQUIRE(history.TryRecord(base.Graph, base.Graph, sourceA, sourceB, allow));
	CHECK(history.CurrentSourceBytes() == sourceB);
	REQUIRE(history.TryRecord(base.Graph, base.Graph, sourceB, sourceC, allow));
	CHECK(history.CurrentSourceBytes() == sourceC);
	Document current = base.Graph;
	REQUIRE(history.Undo(current, allow));
	CHECK(current == base.Graph);
	CHECK(history.CurrentSourceBytes() == sourceB);
	CHECK_FALSE(history.Undo(current, allow));
}
