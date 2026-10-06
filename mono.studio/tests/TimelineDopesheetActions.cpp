#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/TimelineDopesheet.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <iomanip>

TEST_SUITE_ID("studio.timeline_dopesheet_actions")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	Keyframe
	TimedKey(std::string node, uint64_t tick, double value, double fraction = 0, bool negative = false) {
		Keyframe key{std::move(node), "value", tick, value, "linear"};
		key.Subframe = fraction;
		key.NegativeFrame = negative;
		return key;
	}

	struct ScopedCurrentContext {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();

		ScopedCurrentContext() {
			ImGui::SetCurrentContext(Context);
		}
		~ScopedCurrentContext() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		ScopedCurrentContext(const ScopedCurrentContext &) = delete;
		ScopedCurrentContext &operator=(const ScopedCurrentContext &) = delete;
	};

	struct Sheet {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Keys;
		studio::TimelineDopesheet View;
		Diagnostic Error;
		FrameTime Cursor{12, .25, false};
		uint64_t Revision = 0;
		unsigned Changes = 0, Attempts = 0;
		ImVec2 CanvasMin, CanvasMax, TextCenter, ActionButton;
		bool ShowText = false, OpenGatePopup = false, CloseGatePopup = false, GatePopupOpen = false;
		char Text[32] = "editable";
		Sheet() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.FormatVersion = 9;
			Doc.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
			Doc.Outputs = {{"out", "number", "number"}};
			Doc.Keyframes = {
				{"number", "value", 1, 1.0, "linear"},
				{"number", "value", 3, 3.0, "linear"},
				{"number", "value", 7, 7.0, "linear"}
			};
		}
		~Sheet() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Dopesheet", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			if (OpenGatePopup) {
				ImGui::OpenPopup("Delete gate");
				OpenGatePopup = false;
			}
			// Draw's first item is the actual Key actions button. Use its current
			// layout origin and the same default ImGui button sizing calculation.
			const auto actionStart = ImGui::GetCursorScreenPos();
			const auto actionText = ImGui::CalcTextSize("Key actions");
			const auto actionPadding = ImGui::GetStyle().FramePadding;
			ActionButton = {
				actionStart.x + (actionText.x + actionPadding.x * 2) * .5f,
				actionStart.y + (actionText.y + actionPadding.y * 2) * .5f
			};
			View.Draw(
				Doc,
				Revision,
				Keys,
				Cursor,
				Error,
				[&] {
					++Attempts;
					const bool accepted =
						studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &doc) {
							return View.PrepareCommit(doc, Keys, Error);
						});
					if (accepted) {
						View.PublishCommit(Keys);
						++Changes;
						++Revision;
					}
					return accepted;
				},
				[&] {
					++Attempts;
					bool unchanged = false;
					const bool accepted = studio::ApplyImageGraphDocumentEdit(
						Doc,
						History,
						[&](Document &doc) { return Keys.PrepareCommit(doc, Error); },
						&unchanged
					);
					if (accepted || unchanged) Keys.PublishCommit();
					if (accepted) {
						++Changes;
						++Revision;
					}
					return accepted || unchanged;
				}
			);
			CanvasMin = ImGui::GetItemRectMin();
			CanvasMax = ImGui::GetItemRectMax();
			if (ShowText) {
				ImGui::InputText("Text", Text, sizeof(Text));
				const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				TextCenter = {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
			}
			GatePopupOpen = ImGui::BeginPopup("Delete gate");
			if (GatePopupOpen) {
				ImGui::TextUnformatted("Popup owns keyboard input");
				if (CloseGatePopup) {
					ImGui::CloseCurrentPopup();
					CloseGatePopup = false;
				}
				ImGui::EndPopup();
			}
			ImGui::End();
			ImGui::Render();
		}
		void Mouse(ImVec2 point) {
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			Frame();
		}
		void Down(ImVec2 point) {
			Mouse(point);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			Frame();
		}
		void Up() {
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			Frame();
			Frame();
		}
		void Click(size_t index, bool shift = false) {
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, shift);
			Frame();
			Down(View.Markers.at(index).Position);
			Up();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
			Frame();
		}
		void DoubleClick(ImVec2 point) {
			Down(point);
			Up();
			Down(point);
			Up();
		}
		void Chord(ImGuiKey key) {
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			Frame();
			Key(key);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			Frame();
		}
		void FocusTrack(size_t index) {
			Down({CanvasMin.x + 30, View.Markers.at(index).Position.y});
			Up();
		}
		void Key(ImGuiKey key) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
	};
}

TEST_CASE(
	"Dopesheet shortcuts select all channels and paste to the explicitly focused track",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	ui.Doc.Nodes.push_back({"target", "value.number", {}, {}, {{"value", 0.0}}});
	ui.Doc.Nodes.push_back({"other", "value.number", {}, {}, {{"value", 0.0}}});
	ui.Doc.Keyframes.push_back({"other", "value", 2, 22.0, "linear"});
	ui.Doc.Keyframes[0].SourceKeyId = "first-source-key";
	REQUIRE(studio::SetImageGraphKeyframeSourceDriver(ui.Doc, 0, KeyframeLinearDriver{2}, ui.Error));
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers[0].Position);
	const auto before = ui.Doc;
	ui.Chord(ImGuiKey_A);
	REQUIRE(ui.Keys.Selection.size() == 4);
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Keys.Selection = {
		studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[0]),
		studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[1])
	};
	ui.Chord(ImGuiKey_C);
	REQUIRE(ui.Keys.Clipboard.size() == 2);
	CHECK(ui.Keys.Clipboard[0] == before.Keyframes[0]);
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	// Source Ctrl+V requires a focused animator. A hovered key alone is not a target.
	ui.Chord(ImGuiKey_V);
	CHECK(ui.Attempts == 0);
	CHECK(ui.Doc == before);
	// Focus a real displayed track, then paste at the exact signed/fractional owner clock.
	ui.FocusTrack(2);
	REQUIRE(ui.View.FocusedTrack);
	CHECK(ui.View.FocusedTrack->NodeId == "number");
	ui.Chord(ImGuiKey_V);
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Attempts == 1);
	const auto pasted = ui.Doc;
	const auto clone = std::find_if(pasted.Keyframes.begin(), pasted.Keyframes.end(), [](const auto &key) {
		return GetFrameTime(key) == FrameTime{12, .25, false};
	});
	REQUIRE(clone != pasted.Keyframes.end());
	CHECK(clone->NodeId == "number");
	CHECK(clone->SourceKeyId.empty());
	CHECK_FALSE(clone->SourceDriver);
	CHECK(ui.Keys.Clipboard[0] == before.Keyframes[0]);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == pasted);
}

TEST_CASE(
	"Dopesheet Ctrl+D duplicates the complete selection relative to the starting mouse",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	ui.Doc.Keyframes[0].SourceKeyId = "first-source-key";
	REQUIRE(studio::SetImageGraphKeyframeSourceDriver(ui.Doc, 0, KeyframeLinearDriver{2}, ui.Error));
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers[0].Position);
	ui.Chord(ImGuiKey_A);
	const auto before = ui.Doc;
	ui.Chord(ImGuiKey_D);
	REQUIRE(ui.View.Dragging);
	REQUIRE(ui.View.Copying);
	REQUIRE(ui.View.Originals.size() == 3);
	CHECK(ui.Doc == before);
	CHECK(ui.Changes == 0);
	const auto start = ui.View.Markers[0].Position;
	ui.Mouse({start.x + 6 * float(ui.View.PixelsPerFrame), start.y});
	CHECK(ui.Doc == before);
	ui.Down({start.x + 6 * float(ui.View.PixelsPerFrame), start.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Keys.Selection.size() == 3);
	REQUIRE(ui.Doc.Keyframes.size() == 5);
	const auto copied = ui.Doc;
	for (FrameTime destination : std::array<FrameTime, 3>{{{7, 0, false}, {9, 0, false}, {13, 0, false}}}) {
		const auto key = std::find_if(copied.Keyframes.begin(), copied.Keyframes.end(), [&](const auto &k) {
			return GetFrameTime(k) == destination;
		});
		REQUIRE(key != copied.Keyframes.end());
		CHECK(key->SourceKeyId.empty());
		CHECK_FALSE(key->SourceDriver);
	}
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == copied);
}

TEST_CASE(
	"Source quantize uses symmetric ties-to-even and preserves original key payloads",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	ui.Doc.Keyframes = {
		TimedKey("number", 1, 1.0, .5, true), TimedKey("number", 2, 2.0, .5), TimedKey("number", 3, 3.0, .5)
	};
	for (size_t i = 0; i < ui.Doc.Keyframes.size(); ++i)
		ui.Doc.Keyframes[i].SourceKeyId = "source-" + std::to_string(i);
	REQUIRE(studio::SetImageGraphKeyframeSourceDriver(ui.Doc, 0, KeyframeLinearDriver{2}, ui.Error));
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers.back().Position);
	ui.Chord(ImGuiKey_A);
	const auto before = ui.Doc;
	ui.Key(ImGuiKey_Q);
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	const auto quantized = ui.Doc;
	for (size_t i = 0; i < before.Keyframes.size(); ++i) {
		auto expected = before.Keyframes[i];
		REQUIRE(SetFrameTime(
			expected,
			i == 0	 ? FrameTime{2, 0, true}
			: i == 1 ? FrameTime{2, 0, false}
					 : FrameTime{4, 0, false}
		));
		CHECK(
			std::find(quantized.Keyframes.begin(), quantized.Keyframes.end(), expected) !=
			quantized.Keyframes.end()
		);
	}
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == quantized);
}

TEST_CASE(
	"Dopesheet distribute uses the global selection and reverse skips singleton channels",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	ui.Doc.Groups = {{"group", "Group"}};
	ui.Doc.Nodes[0].GroupId = "group";
	ui.Doc.Nodes.push_back({"other", "value.number", "group", {}, {{"value", 0.0}}});
	ui.Doc.Keyframes = {
		TimedKey("number", 2, 2.0, .5, true),
		{"other", "value", 1, 9.0, "linear"},
		TimedKey("number", 5, 5.0, .5),
		{"number", "value", 8, 8.0, "linear"}
	};
	for (size_t i = 0; i < ui.Doc.Keyframes.size(); ++i)
		ui.Doc.Keyframes[i].SourceKeyId = "source-" + std::to_string(i);
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers.back().Position);
	ui.Chord(ImGuiKey_A);
	const auto before = ui.Doc;
	ui.Key(ImGuiKey_D);
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	const auto distributed = ui.Doc;
	const std::array<double, 4> expected{-2.5, 1.0, 4.5, 8.0};
	for (size_t i = 0; i < expected.size(); ++i) {
		const auto key =
			std::find_if(distributed.Keyframes.begin(), distributed.Keyframes.end(), [&](const auto &k) {
				return k.SourceKeyId == before.Keyframes[i].SourceKeyId;
			});
		REQUIRE(key != distributed.Keyframes.end());
		INFO(
			std::setprecision(std::numeric_limits<long double>::max_digits10)
			<< "source=" << key->SourceKeyId << " actual=" << FrameTimeToReal(GetFrameTime(*key)) << " tick="
			<< GetFrameTime(*key).Tick << " fraction=" << key->Subframe << " negative=" << key->NegativeFrame
		);
		CHECK(FrameTimeToReal(GetFrameTime(*key)) == expected[i]);
	}
	ui.Key(ImGuiKey_I);
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 2);
	const auto reversed = ui.Doc;
	const std::array<double, 4> times{8.0, 1.0, 1.0, -2.5};
	for (size_t i = 0; i < times.size(); ++i) {
		const auto key =
			std::find_if(reversed.Keyframes.begin(), reversed.Keyframes.end(), [&](const auto &k) {
				return k.SourceKeyId == before.Keyframes[i].SourceKeyId;
			});
		REQUIRE(key != reversed.Keyframes.end());
		INFO(
			std::setprecision(std::numeric_limits<long double>::max_digits10)
			<< "source=" << key->SourceKeyId << " actual=" << FrameTimeToReal(GetFrameTime(*key)) << " tick="
			<< GetFrameTime(*key).Tick << " fraction=" << key->Subframe << " negative=" << key->NegativeFrame
		);
		CHECK(FrameTimeToReal(GetFrameTime(*key)) == times[i]);
		CHECK(key->Data == before.Keyframes[i].Data);
	}
	CHECK(reversed.Groups == before.Groups);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == distributed);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == distributed);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == reversed);
}

TEST_CASE(
	"Dopesheet alignment collision publishes the first selected identity in one undo",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers[0].Position);
	ui.Chord(ImGuiKey_A);
	ui.Doc.Keyframes[0].SourceKeyId = "first-source-key";
	const auto before = ui.Doc;
	ui.Key(ImGuiKey_A);
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 1);
	CHECK(ui.Doc.Keyframes[0] == before.Keyframes[0]);
	REQUIRE(ui.Keys.Selection.size() == 1);
	const auto aligned = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == aligned);
}

TEST_CASE(
	"Dopesheet keyboard ownership and history refusal preserve document and selection",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	ui.Doc.Keyframes[0].Subframe = .5;
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers[0].Position);
	ui.Chord(ImGuiKey_A);
	const auto before = ui.Doc;
	const auto selection = ui.Keys.Selection;
	SECTION("history refuses the changed action") {
		ui.History = studio::ImageGraphHistory(128, 1);
		ui.Key(ImGuiKey_Q);
		CHECK(ui.Attempts == 1);
		CHECK(ui.Changes == 0);
		CHECK_FALSE(ui.View.Transforming);
	}
	SECTION("text input owns keys") {
		ui.ShowText = true;
		ui.Frame();
		ui.Down(ui.TextCenter);
		ui.Up();
		ui.Key(ImGuiKey_Q);
		CHECK(ui.Attempts == 0);
	}
	SECTION("a real popup owns keys") {
		ui.OpenGatePopup = true;
		ui.Frame();
		ui.Frame();
		REQUIRE(ui.GatePopupOpen);
		ui.Key(ImGuiKey_Q);
		CHECK(ui.Attempts == 0);
	}
	SECTION("unsupported modifiers preserve selection") {
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
		ui.Frame();
		ui.Chord(ImGuiKey_A);
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
		ui.Frame();
		CHECK(ui.Attempts == 0);
	}
	CHECK(ui.Doc == before);
	CHECK(ui.Keys.Selection == selection);
}

TEST_CASE(
	"Source key action destination admission preserves output and exact stale originals",
	"[studio][dopesheet_actions]"
) {
	const std::array<Keyframe, 2> keys{
		{TimedKey("number", 1, 1.0, .5, true), TimedKey("number", 2, 2.0, .5, true)}
	};
	std::vector<FrameTime> output{{9, .25, false}};
	const auto retained = output;
	Diagnostic error;
	CHECK_FALSE(
		studio::PrepareTimelineKeyDestinations(keys, studio::TimelineKeyAction::AlignCenter, output, error, 1)
	);
	CHECK(output == retained);
	CHECK(error.Code == Status::LimitExceeded);
	REQUIRE(
		studio::PrepareTimelineKeyDestinations(
			keys, studio::TimelineKeyAction::AlignCenter, output, error, 4096
		)
	);
	CHECK(output == std::vector<FrameTime>{{2, 0, true}, {2, 0, true}});
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Keys.Selection = {studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[0])};
	REQUIRE(ui.View.BeginAction(ui.Doc, ui.Keys, studio::TimelineKeyAction::Reverse, error));
	ui.Doc.Keyframes[0].Data = 91.0;
	const auto before = ui.Doc;
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](Document &doc) {
		return ui.View.PrepareCommit(doc, ui.Keys, error);
	}));
	CHECK(ui.Doc == before);
	CHECK(error.Code == Status::InvalidValue);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"Dopesheet duplicate refusal preserves an existing redo branch and cancels the ended gesture",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	const auto before = ui.Doc;
	auto edited = before;
	edited.Nodes[0].Values[0].Data = 1.0;
	ui.History = studio::ImageGraphHistory(128, std::max(Write(before).size(), Write(edited).size()));
	REQUIRE(ui.History.TryRecord(before, edited));
	ui.Doc = edited;
	REQUIRE(ui.History.Undo(ui.Doc));
	REQUIRE(ui.Doc == before);
	REQUIRE(ui.History.CanRedo());
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers[0].Position);
	ui.Chord(ImGuiKey_A);
	const auto selection = ui.Keys.Selection;
	ui.Chord(ImGuiKey_D);
	REQUIRE(ui.View.Dragging);
	const auto point = ui.View.Markers[0].Position;
	ui.Mouse({point.x + 12 * float(ui.View.PixelsPerFrame), point.y});
	ui.Down({point.x + 12 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	CHECK(ui.Attempts == 1);
	CHECK(ui.Changes == 0);
	CHECK(ui.Doc == before);
	CHECK(ui.Keys.Selection == selection);
	CHECK_FALSE(ui.View.Dragging);
	REQUIRE(ui.History.CanRedo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == edited);
}

TEST_CASE(
	"Dopesheet menu aligns center and right through real keyboard navigation and one undo",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers[0].Position);
	ui.Chord(ImGuiKey_A);
	const auto before = ui.Doc;
	const char *label = "Align center";
	SECTION("center") {
		label = "Align center";
	}
	SECTION("right") {
		label = "Align right";
	}
	ui.Down(ui.ActionButton);
	ui.Up();
	REQUIRE(!ui.Context->OpenPopupStack.empty());
	auto *popup = ui.Context->OpenPopupStack.back().Window;
	REQUIRE(popup);
	const ImGuiID item = popup->GetID(label);
	for (size_t i = 0; i < 8 && ui.Context->NavId != item; ++i)
		ui.Key(ImGuiKey_DownArrow);
	REQUIRE(ui.Context->NavId == item);
	ui.Key(ImGuiKey_Enter);
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Doc.Keyframes.size() == 1);
	CHECK(
		GetFrameTime(ui.Doc.Keyframes[0]) ==
		FrameTime{std::string_view(label) == "Align center" ? 4u : 7u, 0, false}
	);
	const auto after = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
}

TEST_CASE(
	"Dopesheet label focus refuses insufficient retained payload without "
	"replacing its prior target",
	"[studio][dopesheet_actions]"
) {
	Sheet ui;
	const std::string other(200, 'x');
	ui.Doc.Nodes.push_back({other, "value.number", "", {}, {{"value", 2.0}}});
	ui.Doc.Keyframes.push_back(TimedKey(other, 2, 2.0));
	ui.Frame();
	ui.Frame();
	ui.FocusTrack(0);
	REQUIRE(ui.View.FocusedTrack);
	const auto prior = *ui.View.FocusedTrack;
	const auto held = ui.View.CacheBytes();
	REQUIRE(held);
	ui.Keys.Selection.clear();
	const auto editorAllowance = ui.Keys.Remaining(true, true);
	REQUIRE(editorAllowance);
	const uint64_t spareSelection = ui.Keys.Selection.capacity() * sizeof(studio::ImageGraphKeyframeIdentity);
	REQUIRE(*editorAllowance + spareSelection >= *held);
	ui.Keys.Selection.reserve(
		(*editorAllowance + spareSelection - *held) / sizeof(studio::ImageGraphKeyframeIdentity)
	);
	const auto remaining = ui.Keys.Remaining(true, true);
	REQUIRE(remaining);
	REQUIRE(*remaining >= *held);
	ui.FocusTrack(3);
	CHECK(ui.Error.Code == Status::LimitExceeded);
	REQUIRE(ui.View.FocusedTrack);
	CHECK(ui.View.FocusedTrack->NodeId == prior.NodeId);
	CHECK(ui.View.FocusedTrack->Port == prior.Port);
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"Source distribute crossing zero retains exact unit clock and signed fractional endpoints",
	"[studio][dopesheet_actions]"
) {
	const std::array<Keyframe, 4> keys{
		TimedKey("number", 2, 2.0, .5, true),
		TimedKey("other", 1, 9.0),
		TimedKey("number", 5, 5.0, .5),
		TimedKey("number", 8, 8.0)
	};
	std::vector<FrameTime> destinations;
	Diagnostic error;
	REQUIRE(
		studio::PrepareTimelineKeyDestinations(
			keys, studio::TimelineKeyAction::Distribute, destinations, error, Limits::MaximumEvaluationBytes
		)
	);
	REQUIRE(destinations.size() == 4);
	CHECK(destinations.front() == GetFrameTime(keys.front()));
	CHECK(destinations.back() == GetFrameTime(keys.back()));
	INFO(
		std::setprecision(std::numeric_limits<long double>::max_digits10)
		<< "actual=" << FrameTimeToReal(destinations[1]) << " fraction=" << destinations[1].Subframe
	);
	CHECK(destinations[1] == FrameTime{1, 0, false});
	CHECK(destinations[2] == FrameTime{4, .5, false});
}
