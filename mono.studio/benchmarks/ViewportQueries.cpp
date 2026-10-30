// Real editor picking, dragging and overlay ray consumers in backend-free ImGui frames.
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Paths.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <imgui.h>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Projection.hpp>
#include <vector>

TEST_SUITE_ID("studio.bench.viewportqueries")
TEST_DEPENDS("studio.tools")
TEST_DEPENDS("studio.projection")

namespace studio {
	// This existing friend is defined only here in the benchmark executable.
	struct ViewportCameraProbe {
		using Entity = engine::ecs::Entity;
		using Frame = engine::core::CFrame;
		using Vector = engine::core::Vector3;
		struct PickSnapshot {
			engine::world::WorldId World;
			Entity Owner;
			size_t Count = 0;
		};
		static void Prepare(Editor &editor) {
			editor.ResizeViewports(0);
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Active = editor.Universe->Create({.Name = engine::core::Name("viewport-ray-bench")});
			editor.SelectionWorld = editor.Active;
			editor.CurrentTool = Editor::ToolMode::Select;
			editor.SnapEnabled = false;
			editor.ShowGrid = false;
			editor.ShowFacing = false;
			editor.ShowDirectionGizmo = false;
			editor.ShowCursor = false;
			editor.ViewportHovered = true;
			editor.Universe->Enter(editor.Active, [](engine::ecs::Store &store) {
				engine::scene::InstallServices(store);
			});
		}
		template <class Function> static void Enter(Editor &editor, Function function) {
			editor.Universe->Enter(editor.Active, function);
		}
		static engine::world::WorldId World(Editor &editor) {
			return editor.Active;
		}
		static size_t CountDrawables(Editor &editor) {
			size_t count = 0;
			Enter(editor, [&](engine::ecs::Store &store) {
				store.Each<const engine::scene::Transform, const engine::scene::Bounds>(
					[&](Entity, const auto &, const auto &) { ++count; }
				);
			});
			return count;
		}
		static void AttachFrame(Editor &editor, const Frame &camera, bool drawn = true) {
			auto &slot = editor.Overlays[0];
			slot.List = ImGui::GetForegroundDrawList();
			slot.Drawn = drawn;
			slot.X = 60;
			slot.Y = 25;
			slot.Width = 800;
			slot.Height = 400;
			slot.RenderWidth = 800;
			slot.RenderHeight = 400;
			slot.Presented = true;
			slot.PresentedWorld = editor.Active;
			slot.PresentedFrame = camera;
			slot.PresentedFieldOfView = 1.5707963267948966f;
		}
		static std::optional<engine::core::RayHit>
		Cast(Editor &editor, const engine::core::Ray &ray, std::span<const Entity> ignore) {
			return editor.RaycastWorld(editor.Active, ray, ignore);
		}
		static PickSnapshot Pick(Editor &editor, float x, float y, const PanelProjection &panel) {
			editor.PickInViewport(0, x, y, false, panel);
			return {
				editor.SelectionWorld,
				editor.Selection.empty() ? Entity{} : editor.Selection.front(),
				editor.Selection.size()
			};
		}
		static void Select(Editor &editor, const std::vector<Entity> &entities) {
			editor.Selection = entities;
			editor.SelectionWorld = editor.Active;
			editor.SurfaceDragging = {};
			editor.SurfaceGesture = {};
			editor.BoxSelection = {};
			editor.Dragging = {};
			editor.CurrentTool = Editor::ToolMode::Select;
		}
		static void SeedSurface(
			Editor &editor, const std::vector<Entity> &entities, const std::vector<Frame> &before, bool aligns
		) {
			Select(editor, entities);
			editor.DragAligns = aligns;
			auto &drag = editor.SurfaceDragging;
			drag.Active = true;
			drag.Viewport = 0;
			drag.World = editor.Active;
			editor.Universe->Enter(editor.Active, [&](const engine::ecs::Store &store) {
				drag.StoreIdentity = store.Identity();
			});
			drag.Selection = entities;
			drag.Primary = entities.front();
			drag.Instances = entities;
			drag.Before = before;
			editor.SurfaceGesture.Active = true;
			editor.SurfaceGesture.Viewport = 0;
			editor.SurfaceGesture.World = editor.Active;
		}
		static bool Surface(Editor &editor, const PanelProjection &panel) {
			return editor.DragOnSurface(0, panel);
		}
		static void SeedGizmo(
			Editor &editor, const std::vector<Entity> &entities, const std::vector<Frame> &before, int mode
		) {
			Select(editor, entities);
			const auto tool = mode == 0	  ? Editor::ToolMode::Move
							  : mode == 1 ? Editor::ToolMode::Rotate
										  : Editor::ToolMode::Scale;
			editor.CurrentTool = tool;
			auto &drag = editor.Dragging;
			drag.Axis = mode == 1 ? 2 : 0;
			drag.Viewport = 0;
			drag.Mode = tool;
			drag.Centre = {0, 0, -10};
			drag.Grabbed = 0;
			drag.GrabbedPoint = {1, 0, -10};
			drag.Sides = ScaleSide::Side;
			drag.Sign = 1;
			drag.Instances = entities;
			drag.Before = before;
			drag.BeforeSize.assign(entities.size(), Vector{1, 1, 1});
			drag.BeforePivot.assign(entities.size(), Frame{});
		}
		static bool Gizmo(Editor &editor, const PanelProjection &panel) {
			return editor.DrawGizmo(0, panel);
		}
		static void SeedBox(Editor &editor, glm::vec2 start) {
			Select(editor, {});
			editor.SurfaceGesture.Active = true;
			editor.SurfaceGesture.World = editor.Active;
			editor.SurfaceGesture.Viewport = 0;
			editor.SurfaceGesture.Start = start;
			editor.SurfaceGesture.StartedAt = ImGui::GetTime() - 1;
		}
		static bool Box(Editor &editor, const PanelProjection &panel) {
			return editor.DragSelectionBox(0, panel);
		}
		static bool BoxActive(Editor &editor) {
			return editor.BoxSelection.Active;
		}
		static void SeedCursor(Editor &editor) {
			Select(editor, {});
			editor.CurrentTool = Editor::ToolMode::None;
			editor.ShowCursor = true;
			editor.CursorPosition = {7, 8, 9};
			editor.PendingCursor = {.Viewport = 0, .X = 460, .Y = 225, .Wanted = true};
		}
		static void Overlay(Editor &editor) {
			editor.DrawViewportOverlays();
		}
		static Vector Cursor(Editor &editor) {
			return editor.CursorPosition;
		}
	};
}

namespace {
	using namespace engine;
	using Probe = studio::ViewportCameraProbe;
	using Clock = std::chrono::steady_clock;
	constexpr size_t CAPACITY = 512, CALLS = 64;
	void Require(bool okay, const char *message) {
		if (!okay) throw std::runtime_error(message);
	}
	bool Near(float a, float b) {
		return std::isfinite(a) && std::abs(a - b) < .004f;
	}
	bool Near(const core::Vector3 &a, const core::Vector3 &b) {
		return Near(a.X, b.X) && Near(a.Y, b.Y) && Near(a.Z, b.Z);
	}
	bool SameRotation(const core::CFrame &a, const core::CFrame &b) {
		return Near(a.RightVector(), b.RightVector()) && Near(a.UpVector(), b.UpVector()) &&
			   Near(a.LookVector(), b.LookVector());
	}
	bool Flag(const char *name) {
		const char *raw = std::getenv(name);
		if (!raw) return false;
		const std::string_view value(raw);
		Require(value == "0" || value == "1", "viewport benchmark flags require 0 or 1");
		return value == "1";
	}
	size_t Option(const char *name, size_t fallback, size_t low, size_t high) {
		const char *raw = std::getenv(name);
		if (!raw) return fallback;
		const std::string_view value(raw);
		size_t number = 0;
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
		Require(
			parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && number >= low &&
				number <= high,
			"viewport benchmark option outside bounded range"
		);
		return number;
	}
	struct Context {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Handle = nullptr;
		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
			ImGui::SetCurrentContext(Handle);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1600, 900};
			io.DeltaTime = 1.0f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Context() {
			ImGui::DestroyContext(Handle);
			ImGui::SetCurrentContext(Previous);
		}
		void Begin(ImVec2 mouse = {460, 225}, bool down = false) {
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(mouse.x, mouse.y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			ImGui::NewFrame();
		}
		void End() {
			ImGui::Render();
		}
	};
	studio::PanelProjection Panel(const core::CFrame &frame = {}) {
		scene::Camera lens;
		lens.FieldOfViewRadians = 1.5707963267948966f;
		studio::PanelProjection result;
		result.Matrix = scene::ResolveCamera(frame, lens, 2).ViewProjection;
		result.Eye = frame.Position;
		result.ImageMin = {60, 25};
		result.ImageSize = {800, 400};
		result.RenderSize = {800, 400};
		return result;
	}
	struct Fixture {
		Context ImGui;
		studio::Editor Editor;
		std::vector<ecs::Entity> Members;
		std::vector<core::CFrame> Before;
		ecs::Entity Target, Far, Locked;
		size_t Drawables = 0;
		explicit Fixture(size_t count, bool picker = true) {
			scene::RegisterSceneClasses();
			Probe::Prepare(Editor);
			if (picker && count) {
				Target = Part(core::CFrame({0, 0, -10}), {1, 1, 1});
				Far = Part(core::CFrame({0, 0, -20}), {1, 1, 1});
				Locked = Part(core::CFrame({0, 0, -5}), {1, 1, 1}, true);
				while (Drawables < count) {
					const auto index = Drawables;
					auto frame = core::CFrame::Angles(.2f, .3f, -.1f);
					frame.Position = {256 + float(index % 128) * 8, 30, -256 - float(index / 128) * 8};
					Part(frame, {1, 1, 1});
				}
			}
		}
		ecs::Entity Part(const core::CFrame &frame, core::Vector3 half, bool locked = false) {
			ecs::Entity entity;
			Probe::Enter(Editor, [&](ecs::Store &store) {
				entity = store.CreateInstance(scene::PartClass(), "Drawable");
				store.Set(entity, scene::Transform{frame});
				store.Set(entity, scene::Bounds{half});
				auto visual = *store.Get<scene::Visual>(entity);
				visual.Locked = locked;
				store.Set(entity, visual);
			});
			++Drawables;
			return entity;
		}
		void Restore() {
			Probe::Enter(Editor, [&](ecs::Store &store) {
				for (size_t index = 0; index < Members.size(); ++index) {
					store.Set(Members[index], scene::Transform{Before[index]});
					const core::Vector3 size{2, 2, 2};
					Require(
						store.SetPropertyAuthored(Members[index], core::Name("Size"), &size, sizeof(size)),
						"viewport fixture Size reset refused"
					);
				}
			});
		}
		void Begin(
			const core::CFrame &camera = {}, ImVec2 mouse = {460, 225}, bool down = false, bool drawn = true
		) {
			ImGui.Begin(mouse, down);
			Probe::AttachFrame(Editor, camera, drawn);
		}
	};
	struct Findings {
		size_t Hits = 0, TrueReturns = 0, ChangedMembers = 0, DrawIndices = 0;
	};
	struct Report {
		const char *Row = "";
		size_t Drawables = 0, Selection = 0, Calls = 0, Trials = 0, Warmups = 0;
		bool Memory = false, Heap = false, HitsApplicable = false;
		std::array<uint64_t, CAPACITY> Samples{};
		Findings Actual;
		uint64_t AllocatedBytes = 0, AllocatedBlocks = 0;
		core::HeapTotals LastHeap;
	};
	std::vector<Report> Latest;
	void Print() {
		for (const auto &report : Latest) {
			std::cout << "viewport-queries row=" << report.Row << " drawables=" << report.Drawables
					  << " selected=" << report.Selection << " samples=" << report.Trials
					  << " warmups=" << report.Warmups << " actual_caller_calls=" << report.Calls
					  << " actual_hits="
					  << (report.HitsApplicable ? std::to_string(report.Actual.Hits) : "not_applicable")
					  << " actual_true_returns=" << report.Actual.TrueReturns
					  << " verified_changed_members=" << report.Actual.ChangedMembers
					  << " actual_draw_indices_added=" << report.Actual.DrawIndices
					  << " setup_oracles_imgui_frame=excluded per_pick_selection_snapshot=included "
						 "adapter_adornment_ray=not_separately_measured"
					  << " heap_compiled=" << report.Heap
					  << " timing_valid=" << (!report.Memory && !report.Heap) << " measurement_mode="
					  << (report.Memory ? "memory_diagnostic"
						  : report.Heap ? "heap_instrumented_latency"
										: "heap_off_latency");
			if (report.Memory) {
				if (report.Heap)
					std::cout << " allocated_bytes=" << report.AllocatedBytes
							  << " allocated_blocks=" << report.AllocatedBlocks
							  << " process_live_bytes=" << report.LastHeap.LiveBytes
							  << " process_live_blocks=" << report.LastHeap.LiveBlocks
							  << " process_lifetime_peak_bytes=" << report.LastHeap.PeakBytes
							  << " profiler_overhead_bytes=" << report.LastHeap.OverheadBytes;
				else
					std::cout
						<< " allocated_bytes=unavailable allocated_blocks=unavailable "
						   "process_live_bytes=unavailable process_live_blocks=unavailable "
						   "process_lifetime_peak_bytes=unavailable profiler_overhead_bytes=unavailable";
				std::cout << " heap_scope=sum_measured_caller_windows excludes_gpu_driver=true\n";
				continue;
			}
			auto times = report.Samples;
			std::sort(times.begin(), times.begin() + report.Trials);
			const auto rank = [&](size_t p) { return times[(report.Trials * p + 99) / 100 - 1]; };
			double total = 0;
			for (size_t index = 0; index < report.Trials; ++index)
				total += double(report.Samples[index]);
			std::cout << " batch_p50_ns=" << rank(50) << " batch_p95_ns=" << rank(95)
					  << " batch_p99_ns=" << rank(99) << " batch_max_ns=" << times[report.Trials - 1]
					  << " derived_mean_ns_per_caller=" << total / double(report.Calls)
					  << " quantile=nearest_rank individual_caller_tails=not_measured tail_status="
					  << (report.Trials >= 100 ? "at_least100" : "insufficient_below100") << '\n';
		}
	}
	template <class Setup, class Call, class Verify>
	void Measure(
		Fixture &fixture,
		const char *name,
		size_t calls,
		size_t trials,
		size_t warmups,
		bool memory,
		bool hits,
		Setup setup,
		Call call,
		Verify verify
	) {
		Report report;
		report.Row = name;
		report.Drawables = Probe::CountDrawables(fixture.Editor);
		Require(report.Drawables == fixture.Drawables, "viewport fixture actual drawable count differs");
		report.Selection = fixture.Members.size();
		report.Trials = trials;
		report.Warmups = warmups;
		report.Memory = memory;
		report.Heap = core::HeapProfile::IsCompiledIn();
		report.HitsApplicable = hits;
		setup();
		call();
		(void)verify();
		fixture.ImGui.End();
		for (size_t sample = 0; sample < trials + warmups; ++sample) {
			setup();
			const int indicesBefore = ImGui::GetForegroundDrawList()->IdxBuffer.Size;
			core::HeapTotals before;
			if (memory && report.Heap) before = core::HeapProfile::Totals();
			const auto begin = Clock::now();
			call();
			const auto elapsed =
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
			core::HeapTotals after;
			if (memory && report.Heap) after = core::HeapProfile::Totals();
			const int indicesAfter = ImGui::GetForegroundDrawList()->IdxBuffer.Size;
			auto findings = verify();
			Require(indicesAfter >= indicesBefore, "viewport caller discarded its draw list");
			findings.DrawIndices = static_cast<size_t>(indicesAfter - indicesBefore);
			fixture.ImGui.End();
			if (sample < warmups) continue;
			report.Samples[sample - warmups] = static_cast<uint64_t>(elapsed);
			report.Calls += calls;
			report.Actual.Hits += findings.Hits;
			report.Actual.TrueReturns += findings.TrueReturns;
			report.Actual.ChangedMembers += findings.ChangedMembers;
			report.Actual.DrawIndices += findings.DrawIndices;
			if (memory && report.Heap) {
				Require(
					before.DroppedScopes == after.DroppedScopes, "viewport benchmark heap scopes dropped"
				);
				report.AllocatedBytes += after.TotalBytes - before.TotalBytes;
				report.AllocatedBlocks += after.TotalBlocks - before.TotalBlocks;
				report.LastHeap = after;
			}
		}
		Latest.push_back(report);
	}
	void Picking(size_t count, size_t trials, size_t warmups, bool memory) {
		Fixture fixture(count);
		const auto panel = Panel();
		std::array<std::optional<core::RayHit>, CALLS> actual;
		const std::array<ecs::Entity, 1> ignored{fixture.Target};
		for (int mode = 0; mode < (count ? 3 : 1); ++mode) {
			const core::Ray ray =
				mode == 2 ? core::Ray{{-100, 0, 0}, {0, 0, -1}} : core::Ray{{0, 0, 0}, {0, 0, -1}};
			Measure(
				fixture,
				mode == 0	? "actual_world_pick"
				: mode == 1 ? "actual_world_ignore_target"
							: "actual_world_broad_miss",
				CALLS,
				trials,
				warmups,
				memory,
				true,
				[&] { fixture.Begin({}, {460, 225}, false, false); },
				[&] {
					for (size_t index = 0; index < CALLS; ++index)
						actual[index] = Probe::Cast(
							fixture.Editor,
							ray,
							mode == 1 ? std::span<const ecs::Entity>(ignored) : std::span<const ecs::Entity>{}
						);
				},
				[&] {
					Findings found;
					const bool hit = count && mode != 2;
					for (const auto &result : actual) {
						Require(bool(result) == hit, "editor actual world hit/miss differs");
						if (!result) continue;
						const float distance = mode == 1 ? 19.0f : 9.0f;
						Require(
							result->Id == (mode == 1 ? fixture.Far.Id : fixture.Target.Id) &&
								Near(result->Distance, distance) &&
								Near(result->Position, {0, 0, -distance}) && Near(result->Normal, {0, 0, 1}),
							"editor world analytic owner/surface differs"
						);
						++found.Hits;
					}
					return found;
				}
			);
		}
		std::array<Probe::PickSnapshot, CALLS> picks;
		Measure(
			fixture,
			"actual_pick_hit_miss_selection",
			CALLS,
			trials,
			warmups,
			memory,
			true,
			[&] {
				fixture.Begin({}, {460, 225}, false, false);
				Probe::Select(fixture.Editor, {});
			},
			[&] {
				for (size_t index = 0; index < CALLS; ++index)
					picks[index] = Probe::Pick(fixture.Editor, index % 2 ? 760.0f : 460.0f, 225, panel);
			},
			[&] {
				Findings found;
				for (size_t index = 0; index < CALLS; ++index) {
					const bool hit = count && index % 2 == 0;
					Require(
						picks[index].Count == (hit ? 1u : 0u),
						"editor pick did not preserve each hit/miss selection"
					);
					if (hit) {
						Require(
							picks[index].World == Probe::World(fixture.Editor) &&
								picks[index].Owner == fixture.Target,
							"editor pick selected wrong world/owner"
						);
						++found.Hits;
					}
				}
				return found;
			}
		);
	}
	void Surface(size_t trials, size_t warmups, bool memory, int mode) {
		Fixture fixture(0, false);
		const bool aligns = mode == 1, floor = mode != 2;
		auto first = aligns ? core::CFrame::Angles(0, 0, .3f) : core::CFrame::Angles(0, .4f, 0);
		first.Position = {3, 4, 0};
		fixture.Members.push_back(fixture.Part(first, {1, 1, 1}));
		fixture.Before.push_back(first);
		if (!aligns) {
			auto second = first;
			second.Position.X += 3;
			fixture.Members.push_back(fixture.Part(second, {1, 1, 1}));
			fixture.Before.push_back(second);
		}
		if (floor) fixture.Part(core::CFrame({0, -.5f, 0}), {20, .5f, 20});
		auto camera = core::CFrame::Angles(-1.5707963267948966f, 0, 0);
		camera.Position = {0, 10, 0};
		const auto panel = Panel(camera);
		bool result = false;
		Measure(
			fixture,
			mode == 0	? "actual_surface_carry_group"
			: mode == 1 ? "actual_surface_align"
						: "actual_surface_no_floor_holds",
			1,
			trials,
			warmups,
			memory,
			false,
			[&] {
				fixture.Restore();
				fixture.Begin(camera, {460, 225}, true);
				Probe::SeedSurface(fixture.Editor, fixture.Members, fixture.Before, aligns);
			},
			[&] { result = Probe::Surface(fixture.Editor, panel); },
			[&] {
				Require(result, "actual surface carry lost held gesture");
				Findings found;
				found.TrueReturns = 1;
				Probe::Enter(fixture.Editor, [&](ecs::Store &store) {
					for (size_t index = 0; index < fixture.Members.size(); ++index) {
						const auto &actual = store.Get<scene::Transform>(fixture.Members[index])->Frame;
						auto expected = fixture.Before[index];
						if (floor) {
							expected.Position = {float(index) * 3, 1, 0};
							if (aligns) expected = core::CFrame(expected.Position);
						}
						Require(
							Near(actual.Position, expected.Position) && SameRotation(actual, expected),
							"surface analytic support/rigid frame differs"
						);
						if (floor) ++found.ChangedMembers;
					}
				});
				return found;
			}
		);
	}
	void Gizmo(size_t members, int mode, size_t trials, size_t warmups, bool memory) {
		Fixture fixture(0, false);
		for (size_t index = 0; index < members; ++index) {
			auto frame = mode == 0 ? core::CFrame::Angles(0, .4f, 0) : core::CFrame{};
			frame.Position = {members == 1 ? 0.0f : float(index) / float(members - 1) - .5f, 0, -10};
			fixture.Before.push_back(frame);
			fixture.Members.push_back(fixture.Part(frame, {1, 1, 1}));
		}
		const auto panel = Panel();
		bool result = false;
		int indices = 0;
		Measure(
			fixture,
			mode == 0	? "actual_gizmo_move"
			: mode == 1 ? "actual_gizmo_rotate"
						: "actual_gizmo_scale",
			1,
			trials,
			warmups,
			memory,
			false,
			[&] {
				fixture.Restore();
				fixture.Begin({}, mode == 1 ? ImVec2{460, 205} : ImVec2{500, 225}, true);
				Probe::SeedGizmo(fixture.Editor, fixture.Members, fixture.Before, mode);
				indices = ImGui::GetForegroundDrawList()->IdxBuffer.Size;
			},
			[&] { result = Probe::Gizmo(fixture.Editor, panel); },
			[&] {
				Require(result, "active actual gizmo did not retain drag");
				Findings found;
				found.TrueReturns = 1;
				const int added = ImGui::GetForegroundDrawList()->IdxBuffer.Size - indices;
				Require(added > 0, "actual gizmo produced no draw geometry");
				found.DrawIndices = static_cast<size_t>(added);
				Probe::Enter(fixture.Editor, [&](ecs::Store &store) {
					for (size_t index = 0; index < members; ++index) {
						const auto &actual = store.Get<scene::Transform>(fixture.Members[index])->Frame;
						auto expected = fixture.Before[index];
						if (mode == 0)
							expected.Position.X += 2;
						else if (mode == 1) {
							expected = core::CFrame::Angles(0, 0, 1.5707963267948966f);
							expected.Position = {0, fixture.Before[index].Position.X, -10};
						} else
							expected.Position.X += 1;
						Require(
							Near(actual.Position, expected.Position) && SameRotation(actual, expected),
							"gizmo analytic authored frame differs"
						);
						const auto half = store.Get<scene::Bounds>(fixture.Members[index])->HalfExtent;
						Require(
							Near(half, mode == 2 ? core::Vector3{2, 1, 1} : core::Vector3{1, 1, 1}),
							"gizmo actual size differs"
						);
						if (mode == 2)
							Require(
								Near(store.Get<scene::Collider>(fixture.Members[index])->Extent, half),
								"gizmo failed real Size property collider write"
							);
						++found.ChangedMembers;
					}
				});
				return found;
			}
		);
	}
	void BoxAndCursor(size_t trials, size_t warmups, bool memory) {
		Fixture fixture(8);
		const auto panel = Panel();
		bool result = false;
		for (bool hit : {false, true}) {
			const glm::vec2 start(hit ? 460.0f : 760.0f, 225);
			Measure(
				fixture,
				hit ? "actual_box_gesture_on_target" : "actual_box_gesture_on_empty",
				1,
				trials,
				warmups,
				memory,
				false,
				[&] {
					fixture.Begin({}, {start.x + 20, start.y + 20}, true);
					Probe::SeedBox(fixture.Editor, start);
				},
				[&] { result = Probe::Box(fixture.Editor, panel); },
				[&] {
					Require(
						result == !hit && Probe::BoxActive(fixture.Editor) == !hit,
						"actual box selection gesture ownership differs"
					);
					Findings found;
					found.TrueReturns = result;
					return found;
				}
			);
		}
		Fixture cursor(0, false);
		for (int mode = 0; mode < 3; ++mode) {
			auto camera = core::CFrame::Angles(
				mode == 0	? -1.5707963267948966f
				: mode == 2 ? 1.5707963267948966f
							: 0,
				0,
				0
			);
			camera.Position = {0, 10, 0};
			Measure(
				cursor,
				mode == 0	? "actual_overlay_cursor_ground"
				: mode == 1 ? "actual_overlay_cursor_parallel"
							: "actual_overlay_cursor_behind",
				1,
				trials,
				warmups,
				memory,
				false,
				[&] {
					cursor.Begin(camera);
					Probe::SeedCursor(cursor.Editor);
				},
				[&] { Probe::Overlay(cursor.Editor); },
				[&] {
					Require(
						Near(
							Probe::Cursor(cursor.Editor), mode == 0 ? core::Vector3{} : core::Vector3{7, 8, 9}
						),
						"actual overlay cursor plane oracle differs"
					);
					return Findings{};
				}
			);
		}
	}
	void Run() {
		const bool enabled = Flag("MONO_VIEWPORT_QUERIES_BENCH"),
				   memory = Flag("MONO_VIEWPORT_QUERIES_MEMORY");
		if (!enabled) {
			std::cout << "viewport-queries skipped=opt_in_required\n";
			return;
		}
		const size_t trials = Option("MONO_VIEWPORT_QUERIES_TRIALS", 101, 1, CAPACITY),
					 warmups = Option("MONO_VIEWPORT_QUERIES_WARMUPS", 3, 0, 32);
		static const bool registered = [] {
			Require(std::atexit(Print) == 0, "viewport report registration failed");
			return true;
		}();
		(void)registered;
		Latest.clear();
		Latest.reserve(34);
		studio::SetConfigRoot(core::Paths::Base() / "studio-viewport-benchmark-config");
		for (size_t count : {0u, 8u, 1024u, 8192u})
			Picking(count, trials, warmups, memory);
		for (int mode = 0; mode < 3; ++mode)
			Surface(trials, warmups, memory, mode);
		for (size_t members : {1u, 8u, 64u, 256u})
			for (int mode = 0; mode < 3; ++mode)
				Gizmo(members, mode, trials, warmups, memory);
		BoxAndCursor(trials, warmups, memory);
	}
}

BENCH("Viewport CPU rays | actual picking, surface gestures and active gizmos", 1) {
	Run();
}
