#include "ImageGraphAppendProject.hpp"
#include "ImageGraphHistoryCanvas.hpp"
#include "ImageGraphHistorySource.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <studio/ImageGraph.hpp>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.source_history")
TEST_DEPENDS("engine.imagegraphio.pxcxappend")
TEST_DEPENDS("engine.imagegraph.feedback_host")

namespace {
	using namespace engine::imagegraph;
	using engine::bake::PxcxArchive;
	using engine::imagegraphio::PxcxAppendOptions;
	using engine::imagegraphio::PxcxAppendResult;
	using engine::imagegraphio::PxcxImport;
	using History = studio::ImageGraphHistory;

	PxcxArchive Archive(std::string graph, bool thumbnail = false) {
		PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		if (thumbnail) {
			archive.HasThumbnailBlock = true;
			archive.ThumbnailRgba.assign(256 * 256 * 4, 17);
		}
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
	"collection metadata follows source and native history only after admission",
	"[studio][source_history][collections]"
) {
	const auto destination = Destination();
	const auto incoming = Incoming();
	PxcxAppendResult appended;
	Diagnostic diagnostic;
	PxcxAppendOptions options;
	options.Namespace = "imported";
	REQUIRE(engine::imagegraphio::AppendPxcxProject(destination, incoming, options, appended, diagnostic));
	const auto before = Import(destination);
	const auto sourceBefore = Snapshot(destination.OriginalBytes);
	const auto sourceAfter = Snapshot(appended.Project.Source.OriginalBytes);
	using Metadata = History::CollectionMetadata;
	using Collections = std::shared_ptr<const std::vector<Metadata>>;
	const Collections empty = std::make_shared<const std::vector<Metadata>>();
	const Collections imported = std::make_shared<const std::vector<Metadata>>(
		std::vector<Metadata>{{"collection", R"({"members":["imported:number"]})"}}
	);
	const Collections edited = std::make_shared<const std::vector<Metadata>>(
		std::vector<Metadata>{{"collection", R"({"members":["imported:number"],"label":"edited"})"}}
	);
	const Collections metadataOnly = std::make_shared<const std::vector<Metadata>>(
		std::vector<Metadata>{{"collection", R"({"members":["imported:number"],"label":"metadata only"})"}}
	);
	History history;
	Document current = before.Graph;
	current.Nodes.front().Position = {-13, 29};
	REQUIRE(history.TryRecord(before.Graph, current));
	CHECK_FALSE(history.CurrentCollections());
	const Collections initialCollections = std::make_shared<const std::vector<Metadata>>(
		std::vector<Metadata>{{"destination", R"({"members":["number"]})"}}
	);
	const auto unchanged = Text(current);
	const auto refuseInitialization = [](const Document &,
										 const Document &,
										 const History::SourceSnapshot &,
										 const History::SourceSnapshot &) { return false; };
	CHECK_FALSE(history.TryRecord(
		current,
		current,
		sourceBefore,
		sourceBefore,
		initialCollections,
		initialCollections,
		refuseInitialization
	));
	CHECK_FALSE(history.CurrentSourceBytes());
	CHECK_FALSE(history.CurrentCollections());
	CHECK_FALSE(history.TargetCollections(false));
	CHECK(Text(current) == unchanged);
	const auto acceptInitialization = [](const Document &,
										 const Document &,
										 const History::SourceSnapshot &,
										 const History::SourceSnapshot &) { return true; };
	REQUIRE(history.TryRecord(
		current,
		current,
		sourceBefore,
		sourceBefore,
		initialCollections,
		initialCollections,
		acceptInitialization
	));
	CHECK(history.CurrentCollections() == initialCollections);
	CHECK(history.Undo(current));
	CHECK(Text(current) == unchanged);
	CHECK(history.CurrentCollections() == initialCollections);
	REQUIRE(history.Undo(current));
	CHECK(current == before.Graph);
	CHECK(history.CurrentCollections() == initialCollections);
	REQUIRE(history.Redo(current));
	REQUIRE(history.Redo(current));
	CHECK(history.CurrentCollections() == initialCollections);

	Document afterAppend = appended.Project.Graph;
	afterAppend.Nodes.front().Position = current.Nodes.front().Position;
	int admissions = 0;
	const auto admit = [&](const Document &,
						   const Document &,
						   const History::SourceSnapshot &,
						   const History::SourceSnapshot &) {
		++admissions;
		CHECK(history.CurrentCollections() == initialCollections);
		return true;
	};
	REQUIRE(history.TryRecord(
		current, afterAppend, sourceBefore, sourceAfter, initialCollections, imported, admit
	));
	CHECK(history.CurrentCollections() == imported);

	Document afterEdit = afterAppend;
	afterEdit.Nodes.front().Position = {7, -2};
	REQUIRE(history.TryRecord(afterAppend, afterEdit, sourceAfter, sourceAfter));
	CHECK(history.CurrentCollections() == imported);
	REQUIRE(history.TryRecord(afterEdit, afterEdit, sourceAfter, sourceAfter, imported, edited));
	CHECK(history.CurrentCollections() == edited);
	REQUIRE(history.TryRecord(afterEdit, afterEdit, sourceAfter, sourceAfter, edited, metadataOnly));
	CHECK(history.CurrentCollections() == metadataOnly);
	const auto admitUndo = [&](const Document &,
							   const Document &,
							   const History::SourceSnapshot &,
							   const History::SourceSnapshot &) {
		CHECK(history.TargetCollections(false) == edited);
		CHECK(history.CurrentCollections() == metadataOnly);
		return true;
	};
	CHECK(history.Undo(afterEdit, admitUndo));
	CHECK(history.CurrentCollections() == edited);
	const auto admitRedo = [&](const Document &,
							   const Document &,
							   const History::SourceSnapshot &,
							   const History::SourceSnapshot &) {
		CHECK(history.TargetCollections(true) == metadataOnly);
		CHECK(history.CurrentCollections() == edited);
		return true;
	};
	CHECK(history.Redo(afterEdit, admitRedo));
	CHECK(history.CurrentCollections() == metadataOnly);
	const auto beforeRefusalText = Text(afterEdit);
	CHECK_FALSE(history.TryRecord(
		afterEdit,
		current,
		sourceAfter,
		sourceAfter,
		metadataOnly,
		empty,
		[](const Document &,
		   const Document &,
		   const History::SourceSnapshot &,
		   const History::SourceSnapshot &) { return false; }
	));
	CHECK(Text(afterEdit) == beforeRefusalText);
	CHECK(history.CurrentCollections() == metadataOnly);
	CHECK(history.CanUndo());

	const auto rejectStale = [&](const Document &,
								 const Document &,
								 const History::SourceSnapshot &,
								 const History::SourceSnapshot &) {
		++admissions;
		return true;
	};
	CHECK_FALSE(history.TryRecord(afterEdit, current, sourceAfter, sourceAfter, edited, empty, rejectStale));
	CHECK(admissions == 1);

	const Collections large = [&] {
		auto entries = std::make_shared<std::vector<Metadata>>();
		entries->reserve(64);
		entries->push_back({"large", std::string(4096, 'x')});
		return std::const_pointer_cast<const std::vector<Metadata>>(entries);
	}();
	History limited(8, sourceAfter->capacity() + 4 * Text(afterAppend).size() + 512);
	CHECK_FALSE(limited.TryRecord(
		afterAppend,
		afterEdit,
		sourceAfter,
		sourceAfter,
		imported,
		large,
		[&](const Document &,
			const Document &,
			const History::SourceSnapshot &,
			const History::SourceSnapshot &) {
			++admissions;
			return true;
		}
	));
	CHECK(admissions == 1);
	CHECK_FALSE(limited.CurrentCollections());

	const auto beforeAllocationText = Text(afterEdit);
	CHECK_FALSE(history.TryRecord(
		afterEdit,
		current,
		sourceAfter,
		sourceAfter,
		metadataOnly,
		empty,
		[](const Document &,
		   const Document &,
		   const History::SourceSnapshot &,
		   const History::SourceSnapshot &) -> bool { throw std::bad_alloc{}; }
	));
	CHECK(history.CurrentCollections() == metadataOnly);
	CHECK(history.CanUndo());
	CHECK(Text(afterEdit) == beforeAllocationText);
}

TEST_CASE(
	"shared collection metadata is charged once and survives refused restore admission",
	"[studio][source_history][collections][budget]"
) {
	const auto archive = Destination();
	const auto source = Snapshot(archive.OriginalBytes);
	using Metadata = History::CollectionMetadata;
	using Collections = History::CollectionSnapshot;
	auto entries = std::make_shared<std::vector<Metadata>>();
	entries->reserve(1);
	entries->push_back({"collection", R"({"description":")" + std::string(32 * 1024, 'm') + "\"}"});
	const Collections collections = entries;
	const size_t managerBytes = entries->capacity() * sizeof(Metadata) + entries->front().NodeId.capacity() +
								entries->front().MetadataJson.capacity();
	Document current = Import(archive).Graph;
	const size_t textBytes = Text(current).size();
	const size_t byteCapacity = source->capacity() + managerBytes + 10 * textBytes + 512;
	REQUIRE(byteCapacity < source->capacity() + 2 * managerBytes);
	History history(16, byteCapacity);
	REQUIRE(history.TryRecord(current, current, source, source, collections, collections));
	for (int step = 0; step < 4; ++step) {
		Document next = current;
		next.Nodes.front().Position.X += 1;
		REQUIRE(history.TryRecord(current, next, source, source));
		current = std::move(next);
		CHECK(history.CurrentCollections() == collections);
	}
	for (int step = 0; step < 4; ++step) {
		REQUIRE(history.Undo(current));
		CHECK(history.CurrentCollections() == collections);
	}
	for (int step = 0; step < 4; ++step) {
		REQUIRE(history.Redo(current));
		CHECK(history.CurrentCollections() == collections);
	}
	const auto documentBeforeRefusal = Text(current);
	const auto targetBeforeRefusal = history.TargetCollections(false);
	const auto refuse = [](const Document &,
						   const Document &,
						   const History::SourceSnapshot &,
						   const History::SourceSnapshot &) { return false; };
	CHECK_FALSE(history.Undo(current, refuse));
	CHECK(Text(current) == documentBeforeRefusal);
	CHECK(history.CurrentCollections() == collections);
	CHECK(history.TargetCollections(false) == targetBeforeRefusal);
	const auto failAllocation = [](const Document &,
								   const Document &,
								   const History::SourceSnapshot &,
								   const History::SourceSnapshot &) -> bool { throw std::bad_alloc{}; };
	CHECK_FALSE(history.Undo(current, failAllocation));
	CHECK(Text(current) == documentBeforeRefusal);
	CHECK(history.CurrentCollections() == collections);
	CHECK(history.TargetCollections(false) == targetBeforeRefusal);
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

TEST_CASE(
	"source restore preparation stages the exact target archive projection and thumbnail",
	"[studio][source_history][restore]"
) {
	const auto currentArchive = Destination();
	const auto targetArchive = Archive(
		R"JSON({"attri":{"future_root":{"keep":99}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4},"future_input":{"keep":2}}],"future_node":{"keep":3}}],"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":99}]})JSON",
		true
	);
	const auto current = Snapshot(currentArchive.OriginalBytes);
	const auto target = Snapshot(targetArchive.OriginalBytes);
	const auto currentImport = Import(currentArchive);
	const auto targetImport = Import(targetArchive);
	REQUIRE(currentImport.Graph == targetImport.Graph);
	REQUIRE(current != target);
	Document unsaved = currentImport.Graph;
	unsaved.Nodes.front().Position = {900, -400};
	REQUIRE(unsaved != targetImport.Graph);
	std::optional<PxcxArchive> live = currentArchive;
	studio::detail::ImageGraphHistorySource candidate;
	uint64_t remaining = Limits::MaximumEvaluationBytes;
	Diagnostic error;
	REQUIRE(candidate.Prepare(current, target, live, remaining, error));
	CHECK(candidate.Changed);
	REQUIRE(candidate.Archive);
	CHECK(candidate.Archive->OriginalBytes == targetArchive.OriginalBytes);
	CHECK(candidate.Archive->GraphJson == targetArchive.GraphJson);
	Document expected = targetImport.Graph;
	REQUIRE(Migrate(expected, error) == Status::Ok);
	CHECK(candidate.Projection == expected);
	CHECK(candidate.Projection != unsaved);
	CHECK(candidate.Diagnostics == targetImport.Diagnostics);
	const auto preview = targetImport.ReferencePreview();
	REQUIRE(preview);
	CHECK(candidate.Thumbnail.Width == preview->Width);
	CHECK(candidate.Thumbnail.Height == preview->Height);
	CHECK(candidate.Thumbnail.Pixels == targetArchive.ThumbnailRgba);
	CHECK(candidate.Thumbnail.Hash == preview->Hash);
	const auto archiveBytes = studio::detail::ImageGraphHistoryArchiveBytes(*candidate.Archive);
	const auto projectionBytes = DocumentRetainedPayloadBytes(candidate.Projection);
	REQUIRE(archiveBytes);
	REQUIRE(projectionBytes);
	uint64_t held =
		candidate.Thumbnail.Pixels.capacity() + candidate.Diagnostics.capacity() * sizeof(Diagnostic);
	for (const auto &item : candidate.Diagnostics)
		held += item.NodeId.capacity() + item.Port.capacity() + item.Message.capacity();
	const uint64_t expectedRemaining =
		Limits::MaximumEvaluationBytes - *archiveBytes - *projectionBytes - held;
	CHECK(remaining == expectedRemaining);

	const auto opaqueTarget = Archive(
		R"JSON({"attri":{"future_root":{"keep":100}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4},"future_input":{"keep":2}}],"future_node":{"keep":3}}],"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":100}]})JSON"
	);
	const auto opaqueBytes = Snapshot(opaqueTarget.OriginalBytes);
	CHECK(Import(opaqueTarget).Graph == currentImport.Graph);
	studio::detail::ImageGraphHistorySource opaqueCandidate;
	remaining = Limits::MaximumEvaluationBytes;
	REQUIRE(opaqueCandidate.Prepare(current, opaqueBytes, live, remaining, error));
	CHECK(opaqueCandidate.Changed);
	CHECK(opaqueCandidate.Projection == currentImport.Graph);
	CHECK(opaqueCandidate.Archive->OriginalBytes == opaqueTarget.OriginalBytes);
}

TEST_CASE(
	"source restore preparation refusals preserve candidate fields and the byte allowance",
	"[studio][source_history][restore][atomic]"
) {
	const auto currentArchive = Destination();
	const auto targetArchive = Archive(
		R"JSON({"attri":{"future_root":{"keep":42}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4}}]}]})JSON"
	);
	const auto current = Snapshot(currentArchive.OriginalBytes);
	const auto target = Snapshot(targetArchive.OriginalBytes);
	const auto makeCandidate = [&] {
		studio::detail::ImageGraphHistorySource candidate;
		candidate.Archive = currentArchive;
		candidate.Projection = Import(currentArchive).Graph;
		candidate.Diagnostics = {{Status::UnsupportedExecution, "old", "port", "keep"}};
		candidate.Thumbnail = {1, 1, {1, 2, 3, 4}, 5};
		return candidate;
	};
	const auto assertUnchanged = [&](const studio::detail::ImageGraphHistorySource &candidate,
									 const studio::detail::ImageGraphHistorySource &before,
									 uint64_t beforeRemaining,
									 uint64_t remaining) {
		CHECK(candidate.Changed == before.Changed);
		REQUIRE(candidate.Archive);
		REQUIRE(before.Archive);
		CHECK(candidate.Archive->OriginalBytes == before.Archive->OriginalBytes);
		CHECK(candidate.Archive->GraphJson == before.Archive->GraphJson);
		CHECK(candidate.Projection == before.Projection);
		CHECK(candidate.Diagnostics == before.Diagnostics);
		CHECK(candidate.Thumbnail == before.Thumbnail);
		CHECK(remaining == beforeRemaining);
	};
	const auto runRefusal = [&](const History::SourceSnapshot &liveSource,
								const History::SourceSnapshot &targetSource,
								const std::optional<PxcxArchive> &liveArchive,
								uint64_t initialRemaining) {
		auto candidate = makeCandidate();
		const auto before = candidate;
		const uint64_t remainingBefore = initialRemaining;
		Diagnostic error;
		CHECK_FALSE(candidate.Prepare(liveSource, targetSource, liveArchive, initialRemaining, error));
		assertUnchanged(candidate, before, remainingBefore, initialRemaining);
	};
	std::optional<PxcxArchive> wrongLive = targetArchive;
	runRefusal(current, target, wrongLive, Limits::MaximumEvaluationBytes);
	std::optional<PxcxArchive> correctLive = currentArchive;
	const auto malformed = Snapshot(std::vector<std::byte>{std::byte{0x01}});
	runRefusal(current, malformed, correctLive, Limits::MaximumEvaluationBytes);
	const uint64_t tooSmall = target->capacity() * 2 - 1;
	runRefusal(current, target, correctLive, tooSmall);

	studio::detail::ImageGraphHistorySource unchanged;
	uint64_t remaining = 1234;
	Diagnostic error;
	REQUIRE(unchanged.Prepare(current, current, std::nullopt, remaining, error));
	CHECK_FALSE(unchanged.Changed);
	CHECK_FALSE(unchanged.Archive);
	CHECK(remaining == 1234);
}

TEST_CASE(
	"PXC append cache loading admits source history before publishing either host journal",
	"[studio][source_history][append][cache_group]"
) {
	const auto destination = Archive(
		R"JSON({"attri":{"future_root":{"keep":1}},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"inputs":[{"r":{"d":4}}]},{"id":"existing","type":"Node_Cache_Array","x":0,"y":0,"inputs":[],"attri":{"serialize":true,"cache_group":["number"]}}],"aRegion":[{"l":"old","c":16777215,"fs":-1.5,"fe":4.25,"future":17}]})JSON"
	);
	const auto incoming = Archive(
		R"JSON({"metadata":{"description":"loaded manager","author":"artist"},"nodes":[{"id":"cache","type":"Node_Cache_Array","x":0,"y":0,"inputs":[],"attri":{"serialize":false,"cache_group":["number"]}},{"id":"collection","type":"Node_Collection","x":0,"y":0,"inputs":[],"attri":{"custom_input_list":[],"custom_output_list":[]}}]})JSON"
	);
	const auto sourceBefore = Snapshot(destination.OriginalBytes);
	PxcxAppendOptions options;
	options.Namespace = "loaded";
	PxcxAppendResult appended;
	Diagnostic error;
	REQUIRE(engine::imagegraphio::AppendPxcxProject(destination, incoming, options, appended, error));
	const auto sourceAfter = Snapshot(appended.Project.Source.OriginalBytes);
	REQUIRE(appended.Nodes.size() == 2);
	engine::imagegraphio::PxcxAppendPostLoad postLoad;
	REQUIRE(engine::imagegraphio::PreparePxcxAppendPostLoad(appended, "incoming.pxcx", postLoad, error));
	REQUIRE(postLoad.Collections.size() == 1);
	CHECK_FALSE(postLoad.Source);
	const auto collectionsBefore = std::make_shared<const std::vector<History::CollectionMetadata>>();
	const auto collectionsAfter =
		std::make_shared<const std::vector<History::CollectionMetadata>>(std::move(postLoad.Collections));
	const std::array<std::string_view, 1> owners{appended.Nodes.front().NodeId};
	auto before = Import(destination).Graph;
	before.Outputs = {{"result", "number", "number"}};
	auto after = appended.Project.Graph;
	after.Outputs = before.Outputs;
	Plan plan;
	REQUIRE(Compile(before, plan, error) == Status::Ok);
	for (const bool refuse : {true, false}) {
		CapturedFeedbackHost host;
		EvaluationRequest request;
		REQUIRE(SetFrameTime(request, {1, .25, false}));
		request.SourceCachePlayback =
			SourceCachePlaybackObservation{false, SourceCacheSampling::ObservedFrame, false};
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, .25, false}, 4.25, false, false};
		REQUIRE(host.Prepare(before, plan, 1, 1, request, error, Limits::MaximumEvaluationBytes, "result"));
		REQUIRE(host.PreparedData(1, 1));
		const auto prior = *host.PreparedData(1, 1);
		const auto frame = host.PreparedFrame(1, 1);
		History history(8, refuse ? 1 : 16 * 1024 * 1024);
		int admissions = 0;
		const bool accepted =
			host.RefreshLoadedSourceCacheGroups(after, owners, error, [&](uint64_t remainingBytes) {
				++admissions;
				CHECK(*host.PreparedData(1, 1) == prior);
				CHECK(remainingBytes > sourceBefore->capacity() + sourceAfter->capacity());
				CHECK_FALSE(history.CurrentCollections());
				return history.TryRecord(
					before, after, sourceBefore, sourceAfter, collectionsBefore, collectionsAfter
				);
			});
		CHECK(admissions == 1);
		CHECK(accepted == !refuse);
		CHECK(host.PreparedFrame(1, 1) == frame);
		CHECK(history.CanUndo() == !refuse);
		if (refuse) {
			CHECK(error.Code == Status::LimitExceeded);
			CHECK(*host.PreparedData(1, 1) == prior);
			CHECK_FALSE(history.CurrentSourceBytes());
			CHECK_FALSE(history.CurrentCollections());
			continue;
		}
		CHECK(history.CurrentSourceBytes() == sourceAfter);
		CHECK(history.CurrentCollections() == collectionsAfter);
		CHECK(collectionsAfter->front().NodeId == appended.Nodes.back().NodeId);
		CHECK(collectionsAfter->front().MetadataJson.find("loaded manager") != std::string::npos);
		CHECK(host.PreparedData(1, 1)->Entries == prior.Entries);
		REQUIRE(host.PreparedData(1, 1)->CacheGroups.Owners.size() == 2);
		CHECK(host.PreparedData(1, 1)->CacheGroups.Owners.front() == prior.CacheGroups.Owners.front());
		CHECK(host.PreparedData(1, 1)->CacheGroups.Owners.back().NodeId == owners.front());
		auto current = after;
		REQUIRE(history.Undo(current));
		CHECK(current == before);
		CHECK(history.CurrentSourceBytes() == sourceBefore);
		CHECK(history.CurrentCollections() == collectionsBefore);
		REQUIRE(history.Redo(current));
		CHECK(current == after);
		CHECK(history.CurrentCollections() == collectionsAfter);
		history.Clear();
		CHECK_FALSE(history.CurrentCollections());
	}
}

TEST_CASE(
	"image graph history canvas accounting includes opaque links widgets identifiers and output ports",
	"[studio][source_history][canvas_budget]"
) {
	const auto source =
		Archive(R"JSON({"nodes":[{"id":"future","type":"Vendor_Future","x":0,"y":0,"inputs":[]}]})JSON");
	const auto imported = Import(source);
	REQUIRE(imported.Graph.Nodes.size() == 1);
	CHECK(imported.Graph.Nodes.front().Type == "pxcx.opaque/Vendor_Future");
	Document document = imported.Graph;
	document.Links.push_back({"missing-source", "missing-output", "future", "missing-input"});
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, graph, ids, error));
	REQUIRE(ids.UnmappedLinks == document.Links);
	auto retained = studio::detail::ImageGraphHistoryCanvasBytes(graph, ids);
	REQUIRE(retained);
	const uint64_t baseline = *retained;

	auto *canvasNode = graph.Find(ids.ToCanvas.at("future"));
	REQUIRE(canvasNode);
	nodegraph::Value widget;
	widget.Kind = nodegraph::WidgetKind::Text;
	widget.Text = std::string(1024, 'w');
	canvasNode->Widgets.emplace("notes", std::move(widget));
	retained = studio::detail::ImageGraphHistoryCanvasBytes(graph, ids);
	REQUIRE(retained);
	CHECK(*retained > baseline);
	const uint64_t withWidget = *retained;

	ids.IssuedNodeIds.emplace(std::string(1024, 'i'));
	retained = studio::detail::ImageGraphHistoryCanvasBytes(graph, ids);
	REQUIRE(retained);
	CHECK(*retained > withWidget);
	const uint64_t withIds = *retained;

	canvasNode->OutputPorts =
		std::vector<nodegraph::PortSpec>{{std::string(1024, 'p'), std::string(128, 't')}};
	retained = studio::detail::ImageGraphHistoryCanvasBytes(graph, ids);
	REQUIRE(retained);
	CHECK(*retained > withIds);
	const uint64_t withOutputs = *retained;

	canvasNode->InputPorts =
		std::vector<nodegraph::PortSpec>{{std::string(1024, 'i'), std::string(128, 't')}};
	retained = studio::detail::ImageGraphHistoryCanvasBytes(graph, ids);
	REQUIRE(retained);
	CHECK(*retained > withOutputs);

	nodegraph::Value oversized;
	oversized.Kind = nodegraph::WidgetKind::Text;
	oversized.Text.assign(Limits::MaximumEvaluationBytes + 1, 'x');
	canvasNode->Widgets.emplace("oversized", std::move(oversized));
	CHECK_FALSE(studio::detail::ImageGraphHistoryCanvasBytes(graph, ids));
}

TEST_CASE(
	"Studio append stages unsaved source edits opaque links and metadata before history",
	"[studio][source_history][append_project]"
) {
	using namespace engine::imagegraphio;
	const auto baseline = Destination();
	auto imported = Import(baseline);
	auto live = imported.Graph;
	Diagnostic error;
	REQUIRE(Migrate(live, error) == Status::Ok);
	live.Nodes.front().Values.front().Data = 21.;
	std::vector<std::byte> savedBytes;
	REQUIRE(WritePxcxProjection(imported, live, {}, savedBytes, error));
	PxcxArchive destination;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(savedBytes, destination, failure));
	live.Outputs = {{"preview", "number", "number"}};
	const auto incoming = Archive(
		R"JSON({"metadata":{"description":"incoming manager"},"nodes":[{"id":"root","type":"Node_Collection","x":10,"y":20,"inputs":[],"attri":{"path":"old.pxcx","custom_input_list":[],"custom_output_list":[]}},{"id":"nested","type":"Node_Collection","group":"root","x":3,"y":4,"inputs":[],"attri":{"custom_input_list":[],"custom_output_list":[]}},{"id":"number","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":9}}]},{"id":"foreign","type":"Vendor_Future","x":0,"y":0,"inputs":[{"from_node":"number","from_index":0,"r":{"d":9}}]}]})JSON"
	);
	std::vector<PxcxCollectionMetadata> managers;
	REQUIRE(PreparePxcxCollectionMetadata(baseline, managers, error));
	const auto beforeManagers =
		std::make_shared<const std::vector<PxcxCollectionMetadata>>(std::move(managers));
	studio::ImageGraphCanvasIds ids;
	ids.NextNodeId = 81;
	ids.IssuedNodeIds.insert("retired-node");
	studio::ImageGraphGroupHost host;
	PxcxAppendOptions options;
	options.Namespace = "joined";
	options.Offset = {7, -2};
	studio::detail::ImageGraphAppendProject candidate;
	const bool prepared = candidate.Prepare(
		baseline,
		destination,
		incoming,
		options,
		"incoming.pxcx",
		live,
		host,
		beforeManagers,
		ids,
		2,
		{},
		error
	);
	INFO(error.Message);
	REQUIRE(prepared);
	const auto savedNumber = std::find_if(
		candidate.Groups.Authored.Nodes.begin(), candidate.Groups.Authored.Nodes.end(), [](const auto &node) {
			return node.Id == "number";
		}
	);
	REQUIRE(savedNumber != candidate.Groups.Authored.Nodes.end());
	CHECK(savedNumber->Values.front().Data == Value{21.});
	CHECK(candidate.Groups.Authored.Outputs == live.Outputs);
	CHECK(candidate.Observation.Inputs == candidate.Groups.Authored);
	CHECK(candidate.Observation.Ready);
	CHECK(candidate.Ids.NextNodeId >= 81);
	CHECK(candidate.Ids.IssuedNodeIds.contains("retired-node"));
	CHECK(candidate.Graph.Links().size() == candidate.Groups.Authored.Links.size());
	CHECK(candidate.Selection.size() == 2);
	CHECK(candidate.SelectedGroups == std::vector<std::string>{"joined/root"});
	CHECK(candidate.Ids.EmptyGroups.contains("joined/root"));
	REQUIRE(candidate.Collections);
	REQUIRE(candidate.Collections->size() == 2);
	CHECK(candidate.Collections->front().NodeId == "joined/root");
	CHECK(candidate.Collections->front().MetadataJson.find("incoming manager") != std::string::npos);
	CHECK(candidate.Collections->back().NodeId == "joined/nested");
	CHECK(candidate.Collections->back().MetadataJson.find("incoming manager") == std::string::npos);
	CHECK(candidate.Source.GraphJson.find("old.pxcx") != std::string::npos);
	CHECK(candidate.Source.GraphJson.find("incoming.pxcx") == std::string::npos);
	REQUIRE(candidate.BeforeSource);
	CHECK(*candidate.BeforeSource == baseline.OriginalBytes);
	REQUIRE(candidate.AfterSource);
	CHECK(*candidate.AfterSource == candidate.Source.OriginalBytes);
	CapturedFeedbackHost feedback;
	History refused(8, 1);
	CHECK_FALSE(candidate.Admit(live, refused, feedback, beforeManagers, error));
	CHECK_FALSE(refused.CanUndo());
	History history;
	REQUIRE(candidate.Admit(live, history, feedback, beforeManagers, error));
	CHECK(history.CurrentCollections() == candidate.Collections);
	auto current = candidate.Groups.Authored;
	REQUIRE(history.Undo(current));
	CHECK(current == live);
	CHECK(*history.CurrentSourceBytes() == baseline.OriginalBytes);
	CHECK(history.CurrentCollections() == beforeManagers);
	REQUIRE(history.Redo(current));
	CHECK(current == candidate.Groups.Authored);
	CHECK(history.CurrentCollections() == candidate.Collections);
	// grug a second append keeps earlier managers and source history.
	PxcxAppendOptions next;
	next.Namespace = "second";
	studio::detail::ImageGraphAppendProject second;
	REQUIRE(second.Prepare(
		candidate.Source,
		candidate.Source,
		Incoming(),
		next,
		"other.pxcx",
		current,
		candidate.Groups.Host,
		candidate.Collections,
		candidate.Ids,
		3,
		{},
		error
	));
	REQUIRE(second.Collections);
	CHECK(*second.Collections == *candidate.Collections);
	REQUIRE(second.Admit(current, history, feedback, candidate.Collections, error));
	CHECK(history.CurrentCollections() == second.Collections);
	current = second.Groups.Authored;
	REQUIRE(history.Undo(current));
	CHECK(current == candidate.Groups.Authored);
	CHECK(history.CurrentCollections() == candidate.Collections);
}

TEST_CASE(
	"Studio append refuses malformed source and low allowance without replacing candidate",
	"[studio][source_history][append_project][atomic]"
) {
	const auto baseline = Destination();
	const auto incoming = Incoming();
	const auto live = Import(baseline).Graph;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphCanvasIds ids;
	PxcxAppendOptions options;
	options.Namespace = "check";
	Diagnostic error;
	studio::detail::ImageGraphAppendProject candidate;
	candidate.Source = baseline;
	candidate.Projection = live;
	candidate.Selection = {17};
	candidate.SelectedGroups = {"prior-group"};
	const auto beforeBytes = candidate.Source.OriginalBytes;
	CHECK_FALSE(
		candidate.Prepare(baseline, baseline, incoming, options, {}, live, host, {}, ids, 2, {}, error, 1)
	);
	CHECK(candidate.Source.OriginalBytes == beforeBytes);
	CHECK(candidate.Projection == live);
	CHECK(candidate.Selection == std::vector<nodegraph::NodeId>{17});
	CHECK(candidate.SelectedGroups == std::vector<std::string>{"prior-group"});
	CHECK(candidate.Remaining == 0);
	auto malformed = incoming;
	malformed.GraphJson = "{}";
	CHECK_FALSE(
		candidate.Prepare(baseline, baseline, malformed, options, {}, live, host, {}, ids, 2, {}, error)
	);
	CHECK(candidate.Source.OriginalBytes == beforeBytes);
	CHECK(candidate.Projection == live);
	CHECK(candidate.Selection == std::vector<nodegraph::NodeId>{17});
	CHECK(candidate.SelectedGroups == std::vector<std::string>{"prior-group"});
}

TEST_CASE(
	"Studio single root Collection append retains checked path and nested default managers",
	"[studio][source_history][append_project]"
) {
	const auto destination = Destination();
	const auto incoming = Archive(
		R"JSON({"metadata":{"description":"file manager"},"nodes":[{"id":"root","type":"Node_Collection","x":0,"y":0,"inputs":[],"attri":{"path":"old.pxcx","custom_input_list":[],"custom_output_list":[]}},{"id":"nested","type":"Node_Collection","group":"root","x":0,"y":0,"inputs":[],"attri":{"custom_input_list":[],"custom_output_list":[]}}]})JSON"
	);
	const auto live = Import(destination).Graph;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphCanvasIds ids;
	PxcxAppendOptions options;
	options.Namespace = "single";
	Diagnostic error;
	studio::detail::ImageGraphAppendProject candidate;
	const bool prepared = candidate.Prepare(
		destination, destination, incoming, options, "incoming.pxcx", live, host, {}, ids, 2, {}, error
	);
	INFO(error.Message);
	REQUIRE(prepared);
	CHECK(candidate.Source.GraphJson.find("incoming.pxcx") != std::string::npos);
	REQUIRE(candidate.Collections);
	REQUIRE(candidate.Collections->size() == 2);
	CHECK(candidate.Collections->front().MetadataJson.find("file manager") != std::string::npos);
	CHECK(candidate.Collections->back().MetadataJson.find("file manager") == std::string::npos);
	CHECK(candidate.Selection.empty());
	CHECK(candidate.SelectedGroups == std::vector<std::string>{"single/root"});
	CHECK(candidate.Ids.EmptyGroups.contains("single/root"));
	CHECK(candidate.Projection == Import(candidate.Source).Graph);
}

TEST_CASE(
	"Studio append admits native Group load callbacks with the source archive",
	"[studio][source_history][append_project][group]"
) {
	const auto destination = Destination();
	const auto incoming = Archive(
		R"JSON({"metadata":{"description":"group manager"},"nodes":[{"id":"group","type":"Node_Group","x":0,"y":0,"inputs":[{"r":{"d":["kept",4,true]}}],"attri":{"custom_input_list":["input"],"custom_output_list":["output"],"color_depth":1,"interpolate":0,"oversample":0}},{"id":"input","type":"Node_Group_Input","group":"group","x":0,"y":0,"inputs":[{"r":{"d":0}},{"r":{"d":[0,10]}},{"r":{"d":11}},{},{},{},{},{},{},{},{},{},{},{},{},{}]},{"id":"output","type":"Node_Group_Output","group":"group","x":0,"y":0,"inputs":[{"from_node":"input","from_index":0,"from_tag":0}]}]})JSON"
	);
	const auto live = Import(destination).Graph;
	studio::ImageGraphGroupHost host;
	studio::ImageGraphCanvasIds ids;
	PxcxAppendOptions options;
	options.Namespace = "native";
	Diagnostic error;
	studio::detail::ImageGraphAppendProject candidate;
	const bool prepared =
		candidate.Prepare(destination, destination, incoming, options, {}, live, host, {}, ids, 2, {}, error);
	INFO(error.Message);
	REQUIRE(prepared);
	REQUIRE(candidate.Groups.Host.Replay.Find("native/input"));
	CHECK(candidate.Groups.Host.Replay.InstancesBound());
	CHECK(candidate.Groups.Host.Revision == 2);
	Document saved;
	REQUIRE(candidate.Groups.Host.ProjectForSave(candidate.Groups.Authored, 2, saved, error));
	std::vector<std::byte> written;
	INFO("saved constructor document: " << Text(saved));
	INFO("imported source document: " << Text(Import(candidate.Source).Graph));
	const bool serializable =
		engine::imagegraphio::WritePxcxProjection(Import(candidate.Source), saved, {}, written, error);
	INFO(error.Message);
	REQUIRE(serializable);
	PxcxArchive reopened;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(written, reopened, failure));
	CHECK(Import(reopened).Graph.Groups.size() == 1);
}

TEST_CASE(
	"Studio append source writer reserves scratch before preserving or replacing destination",
	"[studio][source_history][append_project][atomic]"
) {
	const auto source = Destination();
	auto imported = Import(source);
	auto saved = imported.Graph;
	Diagnostic error;
	REQUIRE(Migrate(saved, error) == Status::Ok);
	saved.Nodes.front().Position = {-3, 8};
	auto destination = source;
	CHECK_FALSE(
		studio::detail::PrepareImageGraphAppendDestination(imported, saved, {}, destination, error, 1)
	);
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(destination.OriginalBytes == source.OriginalBytes);
	REQUIRE(studio::detail::PrepareImageGraphAppendDestination(imported, saved, {}, destination, error));
	CHECK(destination.OriginalBytes != source.OriginalBytes);
	CHECK(Import(destination).Graph.Nodes.front().Position == saved.Nodes.front().Position);
	imported.Source.GraphJson.insert(0, " ");
	const auto prior = destination.OriginalBytes;
	CHECK_FALSE(studio::detail::PrepareImageGraphAppendDestination(imported, saved, {}, destination, error));
	CHECK(destination.OriginalBytes == prior);
}

TEST_CASE(
	"Studio append callbacks borrow prepared arguments without changing the argument host",
	"[studio][source_history][append_project][argument]"
) {
	Document graph;
	graph.FormatVersion = 9;
	graph.Nodes = {
		{"argument",
		 "pc.argument",
		 "",
		 {},
		 {{"tag", std::string{"width"}}, {"type", EnumValue{1}}, {"default_value", 3.}}}
	};
	graph.Outputs = {{"result", "argument", "value"}};
	engine::imagegraph::SourceArgumentHost arguments;
	const std::array<AuthoredValue, 1> values{{{"width", 17.}}};
	Diagnostic error;
	REQUIRE(arguments.Prepare(values, Limits::MaximumEvaluationBytes, error) == Status::Ok);
	const auto held = arguments.RetainedBytes();
	studio::detail::ImageGraphAppendHost host;
	host.Arguments = &arguments;
	Plan plan;
	REQUIRE(Compile(graph, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &host;
	EvaluatedValue result;
	REQUIRE(EvaluateValue(graph, plan, "result", request, result, error) == Status::Ok);
	CHECK(result.Data == Value{17.});
	CHECK(arguments.RetainedBytes() == held);
	CHECK(host.Files.SourceArguments.RetainedBytes() < held);
}
