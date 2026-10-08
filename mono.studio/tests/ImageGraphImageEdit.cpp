#include "ImageGraphImageEdit.hpp"

#include "ImageGraphDocumentEdit.hpp"
#include "ImageGraphExportIntent.hpp"
#include "ImageGraphHost.hpp"
#include "ImageGraphImageActions.hpp"
#include "ImageGraphRegionBounds.hpp"
#include "TimelineRegions.hpp"

#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui_internal.h>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("studio.imagegraph_image_edit")
TEST_DEPENDS("engine.imagegraphio.source_image_edit")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	PxcxImport Source() {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			Json{
				{"nodes",
				 Json::array({Json{
					 {"id", "image"},
					 {"type", "Node_Image"},
					 {"x", 4},
					 {"y", 8},
					 {"inputs",
					  Json::array(
						  {Json{{"r", {{"d", "gone-and-ungranted.png"}}}},
						   Json{{"r", {{"d", Json::array({0, 0, 0, 0})}}}}}
					  )},
					 {"attri", {{"cache_use", false}, {"cache_data", ""}, {"future", 17}}},
					 {"future_node", "retained"}
				 }})},
				{"future_project", 43}
			}
				.dump() +
			'\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		REQUIRE(imported.Graph.Nodes.front().Type == "pc.image");
		return imported;
	}
	GroupReplayState Bound(const Document &document, uint64_t revision) {
		GroupReplayState initial, prepared, bound;
		Diagnostic diagnostic;
		REQUIRE(RebindGroupReplay(document, initial, revision, prepared, diagnostic) == Status::Ok);
		REQUIRE(BindGroupReplay(document, {}, prepared, revision, bound, diagnostic) == Status::Ok);
		return bound;
	}
	SourceImageFrameObservation Receipt(const Document &document) {
		SourceImageFrameObservation receipt;
		receipt.Controls.Authored = document.Nodes.front();
		receipt.Controls.Inputs = {{"path", std::string("gone-and-ungranted.png")}};
		receipt.AuthoringRevision = 1;
		receipt.InputRevision = 3;
		Image image;
		image.Width = image.Height = 1;
		image.Pixels = {12, 34, 56, 78};
		receipt.Frames = {image};
		const engine::bake::SpriteCacheFrame frame{1, 1, image.Pixels};
		std::string encoded, failure;
		REQUIRE(
			engine::bake::WriteSpriteCache(
				std::span(&frame, 1),
				engine::bake::SpriteCacheLayout::Rgba8TopDown,
				encoded,
				failure,
				1024 * 1024,
				engine::bake::SpriteCacheShape::Sprite
			)
		);
		receipt.EncodedCache = std::move(encoded);
		receipt.CacheLayout = engine::bake::SpriteCacheLayout::Rgba8TopDown;
		return receipt;
	}
	Json Saved(const std::vector<std::byte> &bytes, PxcxImport &imported) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
		return Json::parse(archive.GraphJson.c_str());
	}
}
TEST_CASE(
	"Studio cache edit saves reloads renders and undoes without original file grants", "[studio][image_cache]"
) {
	const auto imported = Source();
	auto document = imported.Graph;
	auto replay = Bound(document, 1);
	const auto receipt = Receipt(document);
	const auto immutable = receipt.Controls;
	studio::ImageGraphHistory history;
	Diagnostic diagnostic;
	bool changed = false;
	REQUIRE(
		studio::detail::ApplyPreparedImageGraphImage(
			document, history, replay, receipt, {SourceImageAction::Cache, 1, 2, 3}, diagnostic, changed
		)
	);
	REQUIRE(changed);
	CHECK(replay.AuthoringRevision() == 2);
	CHECK(history.CanUndo());
	CHECK(receipt.Controls.Authored == immutable.Authored);
	CHECK(receipt.Controls.Inputs == immutable.Inputs);
	CHECK(receipt.Controls.Tick == immutable.Tick);
	CHECK(receipt.Controls.Subframe == immutable.Subframe);
	CHECK(receipt.Controls.NegativeFrame == immutable.NegativeFrame);
	std::vector<std::byte> bytes;
	const bool written = WritePxcxProjection(imported, document, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	PxcxImport restored;
	const auto json = Saved(bytes, restored);
	CHECK(json["future_project"] == 43);
	CHECK(json["nodes"][0]["future_node"] == "retained");
	CHECK(json["nodes"][0]["attri"]["future"] == 17);
	CHECK(json["nodes"][0]["attri"]["cache_use"] == true);
	CHECK(json["nodes"][0]["attri"]["cache_data"] == *receipt.EncodedCache);
	CHECK(json["nodes"][0]["atomic_game_engine"]["sprite_cache"]["layout"] == "rgba8-top-down");
	auto render = restored.Graph;
	render.Outputs = {{"image", "image", "surface_out"}};
	Plan plan;
	REQUIRE(Compile(render, plan, diagnostic) == Status::Ok);
	studio::detail::ImageGraphHost host;
	EvaluationRequest request;
	request.HostProvider = &host;
	Image output;
	const auto status = Evaluate(render, plan, "image", request, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Width == 1);
	CHECK(output.Height == 1);
	CHECK(output.Pixels == std::vector<uint8_t>{12, 34, 56, 78});
	CHECK(host.RetainedBytes == sizeof(host.Files));
	const auto cached = document;
	REQUIRE(history.Undo(document));
	CHECK(document == imported.Graph);
	std::vector<std::byte> undone;
	REQUIRE(WritePxcxProjection(imported, document, {}, undone, diagnostic));
	CHECK(undone == imported.Source.OriginalBytes);
	REQUIRE(history.Redo(document));
	CHECK(document == cached);
	REQUIRE(WritePxcxProjection(imported, document, {}, undone, diagnostic));
	CHECK(undone == bytes);
}
TEST_CASE(
	"Studio cache history and stale generations refuse before document or replay publication",
	"[studio][image_cache]"
) {
	const auto imported = Source();
	auto document = imported.Graph;
	auto replay = Bound(document, 1);
	auto receipt = Receipt(document);
	const auto retained = replay.RetainedBytes();
	studio::ImageGraphHistory history(4, 1);
	Diagnostic diagnostic;
	bool changed = true;
	CHECK_FALSE(
		studio::detail::ApplyPreparedImageGraphImage(
			document, history, replay, receipt, {SourceImageAction::Cache, 1, 2, 3}, diagnostic, changed
		)
	);
	CHECK_FALSE(changed);
	CHECK(document == imported.Graph);
	CHECK(replay.AuthoringRevision() == 1);
	CHECK(replay.RetainedBytes() == retained);
	CHECK_FALSE(history.CanUndo());
	CHECK(diagnostic.Message.find("history budget") != std::string::npos);
	studio::ImageGraphHistory accepted;
	receipt.InputRevision = 4;
	CHECK_FALSE(
		studio::detail::ApplyPreparedImageGraphImage(
			document, accepted, replay, receipt, {SourceImageAction::Cache, 1, 2, 3}, diagnostic, changed
		)
	);
	CHECK(document == imported.Graph);
	CHECK(replay.RetainedBytes() == retained);
	CHECK_FALSE(accepted.CanUndo());
}
TEST_CASE(
	"Copying immutable normalized image controls has no producer call and admits prior storage",
	"[studio][image_cache]"
) {
	const auto imported = Source();
	auto document = imported.Graph;
	document.Outputs = {{"image", "image", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .25;
	request.NegativeFrame = true;
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "image", request, snapshot, diagnostic) == Status::Ok);
	HostNodeCapture capture;
	REQUIRE(
		studio::detail::PrepareImageGraphImageControls(
			document, request, "image", snapshot, capture, diagnostic
		)
	);
	CHECK(capture.Authored == document.Nodes.front());
	CHECK(capture.Tick == 2);
	CHECK(capture.Subframe == .25);
	CHECK(capture.NegativeFrame);
	CHECK(capture.Outputs.empty());
	CHECK(capture.Images.empty());
	CHECK(capture.ImageArrays.empty());
	const auto previous = capture;
	CHECK_FALSE(
		studio::detail::PrepareImageGraphImageControls(
			document, request, "image", snapshot, capture, diagnostic, snapshot.RetainedBytes()
		)
	);
	CHECK(capture.Authored == previous.Authored);
	CHECK(capture.Inputs == previous.Inputs);
	CHECK(capture.Tick == previous.Tick);
	CHECK(capture.Subframe == previous.Subframe);
	CHECK(capture.NegativeFrame == previous.NegativeFrame);
	CHECK(capture.InputImages.empty());
	CHECK(capture.Outputs.empty());
	CHECK(capture.Images.empty());
	CHECK(capture.ImageArrays.empty());
	CHECK(capture.State == previous.State);
	CHECK(capture.Failure == previous.Failure);
	CHECK(capture.CameraPolicy == previous.CameraPolicy);
	CHECK(capture.CameraRow == previous.CameraRow);
}

namespace {
	Document ImageActionDocument(const Document &source) {
		auto document = source;
		document.Outputs = {{"cache-image-preview", "image", "surface_out"}};
		return document;
	}
	struct ImageActionUi {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		PxcxImport Imported = Source();
		Document Doc = ImageActionDocument(Imported.Graph);
		GroupReplayState Replay = Bound(Doc, 1);
		studio::ImageGraphHistory History;
		studio::detail::ImageGraphExportIntent Intent;
		studio::detail::ImageGraphComposerExports::Observation Observation;
		std::vector<engine::imagegraphexport::GraphImageCacheLayoutObservation> Layouts;
		std::string Message;
		int Layout = 0;
		size_t Attempts = 0, Refreshes = 0;
		studio::TimelineRegions Regions;
		Diagnostic RegionError;
		bool ShowRegions = false;
		uint64_t Revision = 1, InputRevision = 3;
		engine::core::Name Owner{"studio.image-cache-actions.fixture"};
		ImGuiWindow *Window = nullptr;
		ImageActionUi() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {900, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Observation.Revision = Revision;
			Observation.InputRevision = InputRevision;
			Observation.Playback.SourceBounds = SourceAuthoringFrameBounds{};
			Observation.Playback.SelectedRegion =
				std::pair{FrameTime{2, .25, true}, FrameTime{8, .75, false}};
			studio::SetImageGraphAuthorFrame(Observation.Playback, {2, .5, true});
		}
		~ImageActionUi() {
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({800, 450});
			ImGui::Begin("Image cache actions", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			Window = ImGui::GetCurrentWindow();
			studio::detail::DrawImageGraphImageActions(
				Doc.Nodes.front(),
				Layout,
				Message,
				Layouts,
				[&](SourceImageAction action) {
					++Attempts;
					studio::detail::ImageGraphExportIntent::Target target{Doc.Nodes.front().Id, {}, {}, {}};
					target.ImageAction = action;
					(void)Intent.Begin(
						studio::detail::ImageGraphExportIntent::Kind::HostNode,
						Owner,
						Observation,
						std::span(&target, 1),
						false,
						Message
					);
				},
				[&] { ++Refreshes; }
			);
			if (ShowRegions)
				Regions.Draw(Doc, Revision, Observation.Playback, 20, 0, RegionError, [&](const auto &edit) {
					const bool accepted = studio::ApplyImageGraphDocumentEdit(Doc, History, edit);
					if (accepted) {
						++Revision;
						Observation.Revision = Revision;
					}
					return accepted;
				});
			ImGui::End();
			ImGui::Render();
		}
		void Key(ImGuiKey key) {
			ImGui::GetIO().AddKeyEvent(key, true);
			Frame();
			ImGui::GetIO().AddKeyEvent(key, false);
			Frame();
		}
		void Activate(const char *label) {
			Frame();
			Frame();
			const auto id = Window->GetID(label);
			// Initial NavId alone does not enable keyboard activation; Tab enters real navigation.
			Key(ImGuiKey_Tab);
			for (size_t i = 0; i < 40 && Context->NavId != id; ++i)
				Key(ImGuiKey_Tab);
			INFO("Widget " << label << ", navigation visible=" << Context->NavCursorVisible);
			REQUIRE(Context->NavId == id);
			REQUIRE(Context->NavCursorVisible);
			Key(ImGuiKey_Enter);
		}
		bool Apply(SourceImageFrameObservation prepared, Diagnostic &error) {
			if (!Intent.Matches(Owner, Revision, InputRevision)) {
				Intent.Clear();
				return false;
			}
			REQUIRE(Intent.Selected());
			REQUIRE(Intent.Selected()->ImageAction);
			Plan plan;
			const auto compiled = Compile(Doc, plan, error);
			INFO("Image action Compile: " << error.NodeId << '/' << error.Port << ':' << error.Message);
			REQUIRE(compiled == Status::Ok);
			EvaluationRequest request;
			SetFrameTime(request, studio::GetImageGraphFrame(Intent.Current->Observation.Playback));
			EvaluationSnapshot snapshot;
			REQUIRE(
				EvaluateNodeInputs(Doc, plan, Doc.Nodes.front().Id, request, snapshot, error) == Status::Ok
			);
			REQUIRE(
				studio::detail::PrepareImageGraphImageControls(
					Doc, request, Doc.Nodes.front().Id, snapshot, prepared.Controls, error
				)
			);
			prepared.AuthoringRevision = Revision;
			prepared.InputRevision = InputRevision;
			bool changed = false;
			const bool accepted = studio::detail::ApplyPreparedImageGraphImage(
				Doc,
				History,
				Replay,
				prepared,
				{*Intent.Selected()->ImageAction, Revision, Revision + 1, InputRevision},
				error,
				changed
			);
			if (changed) ++Revision;
			Intent.Clear();
			return accepted;
		}
	};
}
TEST_CASE(
	"Actual cache action buttons admit one immutable selected-clock intent and cancel without edits",
	"[studio][image_cache_ui]"
) {
	ImageActionUi ui;
	const auto before = ui.Doc;
	const auto retained = ui.Replay.RetainedBytes();
	ui.Activate("Cache live images");
	INFO("Cache admission: " << ui.Message << ", attempts=" << ui.Attempts);
	REQUIRE(ui.Intent.Current);
	CHECK(ui.Attempts == 1);
	CHECK(ui.Intent.Current->Targets.size() == 1);
	CHECK(ui.Intent.Selected()->ImageAction == SourceImageAction::Cache);
	CHECK(studio::GetImageGraphFrame(ui.Intent.Current->Observation.Playback) == FrameTime{2, .5, true});
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Intent.Current->Observation.Playback) == 7.75);
	ui.Observation.Playback.SelectedRegion.reset();
	ui.Observation.Playback.SourceBounds->End = {SourceFrameBoundPresence::Explicit, {20, 0, false}};
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Intent.Current->Observation.Playback) == 7.75);
	ui.Activate("Cache live images");
	CHECK(ui.Attempts == 2);
	CHECK(ui.Intent.Current->Targets.size() == 1);
	CHECK_FALSE(ui.Message.empty());
	ui.Intent.Clear();
	CHECK_FALSE(ui.Intent.Current);
	CHECK(ui.Doc == before);
	CHECK(ui.Replay.RetainedBytes() == retained);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Activate("Remove cache");
	CHECK(ui.Intent.Selected()->ImageAction == SourceImageAction::RemoveCache);
	++ui.InputRevision;
	Diagnostic error;
	CHECK_FALSE(ui.Apply(Receipt(ui.Doc), error));
	CHECK_FALSE(ui.Intent.Current);
	CHECK(ui.Doc == before);
	CHECK(ui.Replay.RetainedBytes() == retained);
}
TEST_CASE(
	"Real cache button publication is one undo and Remove cache needs no original file",
	"[studio][image_cache_ui]"
) {
	ImageActionUi ui;
	const auto before = ui.Doc;
	ui.Activate("Cache live images");
	Diagnostic error;
	const bool applied = ui.Apply(Receipt(ui.Doc), error);
	INFO(
		"Cache publication: " << ui.Message << ' ' << error.NodeId << '/' << error.Port << ':'
							  << error.Message
	);
	REQUIRE(applied);
	const auto cached = ui.Doc;
	CHECK(cached != before);
	CHECK(ui.Replay.AuthoringRevision() == 2);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == cached);
	ui.Observation.Revision = ui.Revision;
	ui.Activate("Remove cache");
	SourceImageFrameObservation controls;
	controls.Kind = SourceImageFrameKind::ControlsOnly;
	REQUIRE(ui.Apply(std::move(controls), error));
	CHECK(ui.Doc != cached);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == cached);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc != cached);
}
TEST_CASE(
	"Clicked live cache refuses ungranted source files and history without replacing owners",
	"[studio][image_cache_ui]"
) {
	ImageActionUi ui;
	ui.Activate("Cache live images");
	INFO("Cache admission: " << ui.Message << ", attempts=" << ui.Attempts);
	REQUIRE(ui.Intent.Current);
	const auto before = ui.Doc;
	const auto retained = ui.Replay.RetainedBytes();
	SourceImageFrameObservation output;
	std::string failure;
	CHECK_FALSE(
		engine::imagegraphexport::PrepareGraphSourceImages(
			Receipt(ui.Doc).Controls,
			{},
			engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
			1,
			3,
			true,
			output,
			failure
		)
	);
	CHECK_FALSE(failure.empty());
	CHECK(ui.Doc == before);
	CHECK(ui.Replay.RetainedBytes() == retained);
	ui.History = studio::ImageGraphHistory{4, 1};
	Diagnostic error;
	CHECK_FALSE(ui.Apply(Receipt(ui.Doc), error));
	CHECK_FALSE(ui.Intent.Current);
	CHECK(ui.Doc == before);
	CHECK(ui.Replay.RetainedBytes() == retained);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK(error.Message.find("history budget") != std::string::npos);
}
TEST_CASE(
	"Actual saved cache layout controls bind the data hash and revoke only their observation",
	"[studio][image_cache_ui]"
) {
	ImageActionUi ui;
	const auto prepared = Receipt(ui.Doc);
	auto cache = std::find_if(
		ui.Doc.Nodes.front().SourceProperties.begin(),
		ui.Doc.Nodes.front().SourceProperties.end(),
		[](const auto &value) { return value.Port == "cache_data"; }
	);
	REQUIRE(cache != ui.Doc.Nodes.front().SourceProperties.end());
	cache->Data = *prepared.EncodedCache;
	SECTION("source sprites") {}
	SECTION("source frame cache") {
		ui.Doc.Nodes.front().Type = "pc.cache";
		cache->Port = "cache";
	}
	SECTION("source frame cache array") {
		ui.Doc.Nodes.front().Type = "pc.cache_array";
		cache->Port = "cache";
	}
	const auto before = ui.Doc;
	const auto retained = ui.Replay.RetainedBytes();
	ui.Activate("Saved cache layout");
	ui.Key(ImGuiKey_DownArrow);
	ui.Key(ImGuiKey_DownArrow);
	ui.Key(ImGuiKey_DownArrow);
	ui.Key(ImGuiKey_Enter);
	REQUIRE(ui.Layout == 3);
	ui.Activate("Use cache layout");
	REQUIRE(ui.Layouts.size() == 1);
	CHECK(ui.Layouts[0].Layout == engine::bake::SpriteCacheLayout::Bgra8BottomUp);
	const auto hash = engine::bake::SpriteCacheDataHash(*prepared.EncodedCache);
	REQUIRE(hash);
	CHECK(ui.Layouts[0].DataHash == std::string(hash->data(), hash->size()));
	CHECK(ui.Refreshes == 1);
	CHECK_FALSE(ui.Intent.Current);
	CHECK(ui.Doc == before);
	CHECK(ui.Replay.RetainedBytes() == retained);
	ui.Activate("Revoke cache layout");
	CHECK(ui.Layouts.empty());
	CHECK(ui.Refreshes == 2);
	CHECK(ui.Doc == before);
}
TEST_CASE(
	"The real Match live image count button stages live length and retains source endpoint priority",
	"[studio][image_cache_ui]"
) {
	ImageActionUi ui;
	ui.Doc.Nodes = {
		{"image",
		 "pc.image_animated",
		 "",
		 {},
		 {{"path",
		   ArrayValue{
			   ValueType::Text,
			   {std::string("gone-and-ungranted.png"), std::string("second-and-ungranted.png")}
		   }}}}
	};
	ui.Doc.Outputs = {{"image-preview", "image", "surface_out"}};
	ui.Doc.Keyframes.clear();
	ui.Doc.Tracks.clear();
	ui.Doc.Timeline = TimelineSettings{20, 0, 19, "stop", 30, SourceAuthoringFrameBounds{}};
	ui.Doc.Timeline->SourceBounds->End = {SourceFrameBoundPresence::Explicit, {12, 0, false}};
	ui.Replay = Bound(ui.Doc, 1);
	const auto before = ui.Doc;
	ui.Activate("Match live image count");
	CHECK(ui.Intent.Selected()->ImageAction == SourceImageAction::MatchLength);
	auto prepared = Receipt(ui.Doc);
	prepared.Frames.push_back(prepared.Frames.front());
	prepared.EncodedCache.reset();
	prepared.CacheLayout.reset();
	Diagnostic error;
	const bool accepted = ui.Apply(std::move(prepared), error);
	INFO(error.Message);
	REQUIRE(accepted);
	CHECK(ui.Doc.Timeline->Frames == 2);
	CHECK(ui.Doc.Timeline->SourceBounds == before.Timeline->SourceBounds);
	CHECK(ui.Doc.Timeline->SourceBounds->End.Value == FrameTime{12, 0, false});
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc.Timeline->Frames == 2);
}

TEST_CASE(
	"A real source range reset invalidates a frozen cache intent instead of relabelling its selected bounds",
	"[studio][image_cache_ui]"
) {
	ImageActionUi ui;
	ui.Doc.Timeline = TimelineSettings{20, 4, 12, "stop", 30, {}};
	if (!ui.Doc.Project) ui.Doc.Project.emplace();
	ui.Doc.Project->AnimationRegions = {{"chosen", {255, 255, 255, 255}, {2, .25, true}, {8, .75, false}}};
	studio::ApplyImageGraphTimeline(ui.Doc, ui.Observation.Playback);
	ui.Regions.Synchronize(ui.Doc, ui.Revision, ui.Observation.Playback);
	ui.Regions.Selected = 0;
	ui.Regions.Bind(ui.Doc, ui.Observation.Playback);
	ui.ShowRegions = true;
	ui.Activate("Cache live images");
	INFO("Cache admission: " << ui.Message << ", attempts=" << ui.Attempts);
	REQUIRE(ui.Intent.Current);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Intent.Current->Observation.Playback) == 12);
	const auto before = ui.Doc;
	ui.Activate("Clear source range");
	REQUIRE(ui.Doc.Timeline->SourceBounds);
	CHECK(ui.Revision == 2);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Observation.Playback) == 7.75);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Intent.Current->Observation.Playback) == 12);
	CHECK_FALSE(ui.Intent.Matches(ui.Owner, ui.Revision, ui.InputRevision));
	Diagnostic error;
	CHECK_FALSE(ui.Apply(Receipt(ui.Doc), error));
	CHECK_FALSE(ui.Intent.Current);
	CHECK(ui.Replay.AuthoringRevision() == 1);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
}
