#include "Vector2Panel.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>

namespace studio {
	using engine::imagegraph::Status;
	using engine::imagegraph::Vector2;
	namespace {
		constexpr size_t SpriteBudget = engine::imagegraph::Limits::MaximumEvaluationBytes;
		ImVec2 Point(Vector2 p) {
			return {static_cast<float>(p.X), static_cast<float>(p.Y)};
		}
		bool Coordinate(const engine::imagegraph::Vector2Presentation &v) {
			return v.DisplayType == 1 && !v.XLinked && !v.YLinked && v.ProcessorCount == 1;
		}
	}
	std::string Vector2Panel::Id(nodegraph::NodeId canvas) const {
		if (!Ids) return {};
		const auto found = Ids->ToDocument.find(canvas);
		return found == Ids->ToDocument.end() ? std::string{} : found->second;
	}
	const engine::imagegraph::Vector2Presentation *Vector2Panel::Presentation(std::string_view node) const {
		const auto found = Rows.find(std::string(node));
		return found == Rows.end() || !found->second.Valid ? nullptr : &found->second.Value;
	}
	void Vector2Panel::Attach(
		nodegraph::Canvas &canvas,
		engine::imagegraph::Document &document,
		ImageGraphCanvasIds &ids,
		ImageGraphHistory &history,
		std::function<void()> changed
	) {
		End();
		Document = &document;
		Ids = &ids;
		History = &history;
		Changed = std::move(changed);
		canvas.Signals.MeasureBody = [this](const nodegraph::Node &node) {
			if (node.Type != "pc.vector2") return nodegraph::BodySize{};
			const auto *value = Presentation(Id(node.Id));
			const auto row = Rows.find(Id(node.Id));
			return value && Coordinate(*value) && row != Rows.end() && row->second.ScalarAxes
					   ? nodegraph::BodySize{160, 160}
					   : nodegraph::BodySize{96, 80};
		};
		canvas.Signals.InputBody = [this](const auto &node, const auto &frame) {
			return InputBody(node, frame);
		};
		canvas.Signals.DrawBody = [this](const auto &node, const auto &frame) { DrawBody(node, frame); };
	}
	void Vector2Panel::Begin(std::string_view node, bool overlay) {
		if (!Document || !GestureNode.empty()) return;
		GestureNode = node;
		OverlayDragging = overlay;
		Before = *Document;
		const auto row = Rows.find(std::string(node));
		if (row != Rows.end()) {
			GestureTick = row->second.Tick;
			GestureSubframe = row->second.Subframe;
			GestureNegativeFrame = row->second.NegativeFrame;
		}
	}
	void Vector2Panel::End() {
		if (Before && Document && History && !History->TryRecord(*Before, *Document)) {
			*Document = std::move(*Before);
			Error = {
				engine::imagegraph::Status::LimitExceeded,
				GestureNode,
				{},
				"vector gesture cannot retain its undo snapshot"
			};
			if (Changed) Changed();
		}
		Before.reset();
		GestureNode.clear();
		Panning = false;
		OverlayDragging = false;
	}
	Vector2PreviewSnap Vector2Panel::CurrentSnap() const {
		if (!Document || !Document->Project) return {};
		const auto &p = *Document->Project;
		return {
			p.PreviewGrid.Show, p.PreviewGrid.Snap, p.PreviewGrid.Size, p.ShowPreviewRulers, p.PreviewRulers
		};
	}
	void Vector2Panel::FinishSettings() {
		if (SettingsBefore && Document && History && !History->TryRecord(*SettingsBefore, *Document)) {
			*Document = std::move(*SettingsBefore);
			Error = {
				engine::imagegraph::Status::LimitExceeded,
				{},
				{},
				"preview settings cannot retain their undo snapshot"
			};
			if (Changed) Changed();
		}
		SettingsBefore.reset();
		SettingsId = 0;
	}
	void Vector2Panel::FinishPointer(bool leftDown, bool middleDown) {
		if (SettingsBefore && (!GImGui || GImGui->ActiveId != SettingsId)) FinishSettings();
		if (!GestureNode.empty() && (Panning ? !middleDown : !leftDown)) End();
	}

	bool Vector2Panel::Edit(std::string_view node, Vector2 coordinate) {
		if (!Document) return false;
		const auto target =
			std::find_if(Document->Nodes.begin(), Document->Nodes.end(), [&](const auto &item) {
				return item.Id == node;
			});
		const auto row = Rows.find(std::string(node));
		if (row == Rows.end()) return false;
		const auto current = [&](std::string_view port) -> const engine::imagegraph::Value * {
			bool animated = false;
			for (const auto &key : Document->Keyframes) {
				if (key.NodeId != node || key.Port != port) continue;
				animated = true;
				if (engine::imagegraph::GetFrameTime(key) ==
					engine::imagegraph::FrameTime{GestureTick, GestureSubframe, GestureNegativeFrame})
					return &key.Data;
			}
			if (animated || target == Document->Nodes.end()) return nullptr;
			const auto value = std::find_if(target->Values.begin(), target->Values.end(), [&](const auto &v) {
				return v.Port == port;
			});
			return value == target->Values.end() ? nullptr : &value->Data;
		};
		const auto *x = current("x"), *y = current("y");
		if (x && y && std::get_if<double>(x) && std::get_if<double>(y) &&
			std::get<double>(*x) == coordinate.X && std::get<double>(*y) == coordinate.Y)
			return true;
		if (!SetImageGraphVector2CoordinatesAtFrame(
				*Document, node, coordinate, GestureTick, GestureSubframe, Error, GestureNegativeFrame
			))
			return false;
		if (Before && *Before == *Document) return true;
		if (Changed) Changed();
		return true;
	}
	bool Vector2Panel::InputBody(const nodegraph::Node &node, const nodegraph::BodyFrame &frame) {
		if (node.Type != "pc.vector2") return false;
		const auto id = Id(node.Id);
		auto found = Rows.find(id);
		if (found == Rows.end() || !found->second.Valid || !found->second.ScalarAxes ||
			!Coordinate(found->second.Value))
			return false;
		auto &row = found->second;
		const Vector2EditorRect rect{frame.X, frame.Y, frame.Width, frame.Height};
		const bool active = GestureNode == id && !OverlayDragging;
		if (frame.Hovered && GestureNode.empty()) {
			if (frame.LeftPressed)
				Begin(id, false);
			else if (frame.MiddlePressed) {
				GestureNode = id;
				Panning = true;
			} else if (frame.Wheel != 0)
				ZoomVector2Pad(row.View, frame.Wheel);
			if (frame.RightPressed) MenuNode = id;
		}
		if (GestureNode == id && !OverlayDragging) {
			if (Panning) {
				PanVector2Pad(row.View, {frame.DeltaX, frame.DeltaY}, rect);
				if (!frame.MiddleDown) End();
			} else {
				Vector2 coordinate;
				if (Vector2PadCoordinate(
						row.View, rect, {frame.MouseX, frame.MouseY}, frame.Control, coordinate
					))
					Edit(id, coordinate);
				if (frame.LeftReleased || !frame.LeftDown) End();
			}
		}
		return frame.Hovered || active || GestureNode == id;
	}
	void Vector2Panel::DrawBody(const nodegraph::Node &node, const nodegraph::BodyFrame &frame) {
		if (node.Type != "pc.vector2") return;
		const std::string id = Id(node.Id);
		const auto found = Rows.find(id);
		if (found == Rows.end() || !found->second.Valid) return;
		const auto &row = found->second;
		auto *draw = ImGui::GetWindowDrawList();
		const ImVec2 min(frame.X, frame.Y), max(frame.X + frame.Width, frame.Y + frame.Height);
		draw->PushClipRect(min, max, true);
		char text[128];
		std::snprintf(text, sizeof(text), "[%.9g, %.9g]", row.Value.X, row.Value.Y);
		if (Coordinate(row.Value) && row.ScalarAxes) {
			const auto &view = row.View;
			const double step =
				std::pow(5, std::floor(std::log(view.MaximumX - view.MinimumX) / std::log(5)));
			for (double x = std::ceil(view.MinimumX / step) * step; x < view.MaximumX; x += step) {
				const float at =
					frame.X +
					static_cast<float>((x - view.MinimumX) / (view.MaximumX - view.MinimumX)) * frame.Width;
				draw->AddLine({at, frame.Y}, {at, max.y}, x == 0 ? 0x4DFFFFFF : 0x1AFFFFFF);
			}
			for (double y = std::ceil(view.MinimumY / step) * step; y < view.MaximumY; y += step) {
				const float at =
					max.y -
					static_cast<float>((y - view.MinimumY) / (view.MaximumY - view.MinimumY)) * frame.Height;
				draw->AddLine({frame.X, at}, {max.x, at}, y == 0 ? 0x4DFFFFFF : 0x1AFFFFFF);
			}
			if (row.Value.X >= view.MinimumX && row.Value.X <= view.MaximumX &&
				row.Value.Y >= view.MinimumY && row.Value.Y <= view.MaximumY) {
				const ImVec2 at(
					frame.X +
						static_cast<float>((row.Value.X - view.MinimumX) / (view.MaximumX - view.MinimumX)) *
							frame.Width,
					max.y -
						static_cast<float>((row.Value.Y - view.MinimumY) / (view.MaximumY - view.MinimumY)) *
							frame.Height
				);
				draw->AddCircleFilled(at, 3 * frame.Scale, 0xFFFFFFFF);
			}
			draw->AddRect(min, max, 0x80FFFFFF);
			draw->AddText(
				nullptr,
				std::max(5.f, 12 * frame.Scale),
				{frame.X + 4 * frame.Scale, max.y - 16 * frame.Scale},
				0xFFFFFFFF,
				text
			);
		} else {
			if (row.Value.ProcessorCount > 1)
				std::snprintf(
					text, sizeof(text), "%llu rows", static_cast<unsigned long long>(row.Value.ProcessorCount)
				);
			else if (!row.ScalarAxes)
				std::snprintf(text, sizeof(text), "Array controls");
			else
				std::snprintf(text, sizeof(text), "%.9g\n%.9g", row.Value.X, row.Value.Y);
			draw->AddText(
				nullptr,
				std::max(5.f, 14 * frame.Scale),
				{frame.X + 4 * frame.Scale, frame.Y + 8 * frame.Scale},
				0xFFFFFFFF,
				text
			);
		}
		draw->PopClipRect();
		ImGui::PushID(id.c_str());
		if (MenuNode == id) {
			ImGui::OpenPopup("Coordinate view");
			MenuNode.clear();
		}
		if (ImGui::BeginPopup("Coordinate view")) {
			if (ImGui::MenuItem("Reset view")) Rows.at(id).View = {};
			if (ImGui::MenuItem("Focus value")) FocusVector2Pad(Rows.at(id).View, {row.Value.X, row.Value.Y});
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	size_t Vector2Panel::HeldSpriteBytes() const {
		size_t bytes = 0;
		for (const auto &item : PendingDrop)
			bytes += item.second;
		for (const auto &[id, row] : Rows) {
			(void)id;
			bytes += row.TextureBytes + row.RetiredBytes;
			if (row.Value.Sprite) bytes += row.Value.Sprite->Pixels.size();
		}
		return bytes;
	}
	void Vector2Panel::PumpRetired(engine::render::Renderer &renderer) {
		for (const auto &item : PendingDrop)
			renderer.DropTexture(item.first);
		PendingDrop.clear();
		for (auto &[id, row] : Rows) {
			(void)id;
			if (row.Retired.IsValid()) renderer.DropTexture(row.Retired);
			row.Retired = {};
			row.RetiredBytes = 0;
		}
	}
	void Vector2Panel::Refresh(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::EvaluationRequest &request,
		uint64_t revision,
		uint64_t inputs
	) {
		ENGINE_PROFILE("image composer vector controls");
		Error = {};
		for (auto iter = Rows.begin(); iter != Rows.end();) {
			const bool live =
				std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == iter->first && node.Type == "pc.vector2";
				});
			if (live) {
				++iter;
				continue;
			}
			if (iter->first == GestureNode) End();
			if (iter->second.Texture.IsValid())
				PendingDrop.emplace_back(iter->second.Texture, iter->second.TextureBytes);
			if (iter->second.Retired.IsValid())
				PendingDrop.emplace_back(iter->second.Retired, iter->second.RetiredBytes);
			iter = Rows.erase(iter);
		}
		for (const auto &node : document.Nodes) {
			if (node.Type != "pc.vector2") continue;
			auto &row = Rows.try_emplace(node.Id).first->second;
			if (row.Revision == revision && row.Inputs == inputs && row.Tick == request.Tick &&
				row.Subframe == request.Subframe && row.NegativeFrame == request.NegativeFrame)
				continue;
			row.Revision = revision;
			row.Inputs = inputs;
			row.Tick = request.Tick;
			row.Subframe = request.Subframe;
			row.NegativeFrame = request.NegativeFrame;
			row.Valid = false;
			row.Error = {};
			// A pad gesture cannot replace an authored outer array with one scalar sample.
			row.ScalarAxes = std::none_of(node.Values.begin(), node.Values.end(), [](const auto &value) {
				return (value.Port == "x" || value.Port == "y") &&
					   std::holds_alternative<engine::imagegraph::ArrayValue>(value.Data);
			});
			engine::imagegraph::Vector2Presentation candidate;
			engine::imagegraph::Diagnostic diagnostic;
			if (!ReserveVector2Sprite(HeldSpriteBytes(), 128 * 128 * 4, 1, SpriteBudget)) {
				row.Error = {
					Status::LimitExceeded, node.Id, "gizmo_sprite", "gizmo sprites exceed the preview budget"
				};
				continue;
			}
			auto bounded = request;
			if (request.MaximumImageDimension == 0 ||
				request.MaximumImageDimension > engine::imagegraph::Limits::MaximumDimension) {
				row.Error = {Status::InvalidValue, node.Id, {}, "preview image limit is invalid"};
				continue;
			}
			bounded.MaximumImageDimension =
				std::min(request.MaximumImageDimension, IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION);
			if (engine::imagegraph::ResolveVector2Presentation(
					document, node.Id, bounded, candidate, diagnostic
				) != Status::Ok) {
				row.Error = std::move(diagnostic);
				continue;
			}
			const size_t bytes = candidate.Sprite ? candidate.Sprite->Pixels.size() : 0;
			// Keep the old CPU/texture, candidate, upload staging and replacement texture in the same budget.
			if (bytes > 128 * 128 * 4 || !ReserveVector2Sprite(HeldSpriteBytes(), bytes, 3, SpriteBudget)) {
				row.Error = {
					Status::LimitExceeded, node.Id, "gizmo_sprite", "gizmo sprites exceed the preview budget"
				};
				continue;
			}
			row.Value = std::move(candidate);
			row.Valid = true;
			if (!row.Value.Sprite || row.Value.Style != 2 || row.Value.ProcessorCount > 1) {
				if (row.Texture.IsValid()) {
					row.Retired = row.Texture;
					row.RetiredBytes = row.TextureBytes;
					row.Texture = {};
					row.TextureBytes = 0;
				}
			}
		}
		for (const auto &[id, row] : Rows) {
			(void)id;
			if (row.Error.Code != Status::Ok) Error = row.Error;
		}
		if (!GestureNode.empty()) {
			const auto *value = Presentation(GestureNode);
			if (!value || (OverlayDragging && value->ProcessorCount > 1) ||
				(!OverlayDragging && !Panning && (!Coordinate(*value) || !Rows.at(GestureNode).ScalarAxes)))
				End();
		}
	}
	bool Vector2Panel::Upload(Row &row, std::string_view node, engine::render::Renderer &renderer) {
		if (!row.Value.Sprite) return false;
		const auto &image = *row.Value.Sprite;
		if (row.Texture.IsValid() && row.TextureHash == image.Hash) return true;
		if (image.Width == 0 || image.Height == 0 || image.Width > 128 || image.Height > 128 ||
			image.Pixels.size() != uint64_t(image.Width) * image.Height * 4 ||
			!ReserveVector2Sprite(HeldSpriteBytes(), image.Pixels.size(), 2, SpriteBudget)) {
			Error = {
				Status::LimitExceeded,
				std::string(node),
				"gizmo_sprite",
				"gizmo sprite exceeds the preview budget"
			};
			return false;
		}
		engine::assets::TextureData texture;
		texture.Width = image.Width;
		texture.Height = image.Height;
		texture.Format = engine::assets::TextureFormat::RGBA8;
		texture.Pixels.resize(image.Pixels.size());
		std::memcpy(texture.Pixels.data(), image.Pixels.data(), image.Pixels.size());
		const uint64_t slot = row.TextureSerial == 1 ? 2 : 1;
		engine::core::Name name(
			"studio.imagecomposer.vector2/" + std::string(node) + "/" + std::to_string(slot)
		);
		if (!renderer.AddTexture(name, texture)) {
			Error = {
				Status::UnsupportedExecution,
				std::string(node),
				"gizmo_sprite",
				"gizmo sprite texture is unavailable"
			};
			return false;
		}
		row.Retired = row.Texture;
		row.RetiredBytes = row.TextureBytes;
		row.TextureSerial = slot;
		row.Texture = name;
		row.TextureBytes = image.Pixels.size();
		row.TextureHash = image.Hash;
		return true;
	}
	void Vector2Panel::DrawSnapControls() {
		if (!Document || !ImGui::TreeNode("Grid / guides")) return;
		const auto commit = [&](bool changed, const auto &edit) {
			const auto id = ImGui::GetItemID();
			if (ImGui::IsItemActivated()) {
				FinishSettings();
				SettingsBefore = *Document;
				SettingsId = id;
			}
			if (changed) {
				if (!SettingsBefore) {
					SettingsBefore = *Document;
					SettingsId = id;
				}
				if (Document->Project && (Document->Project->Palette.size() >
											  engine::imagegraph::Limits::MaximumProjectPaletteEntries ||
										  Document->Project->PreviewRulers.size() >
											  engine::imagegraph::Limits::MaximumArrayElements)) {
					Error = {
						Status::LimitExceeded, {}, "preview_grid", "project settings exceed their limits"
					};
				} else {
					auto candidate =
						Document->Project ? *Document->Project : engine::imagegraph::ProjectSettings{};
					edit(candidate);
					if (SetImageGraphProjectSettings(*Document, candidate, Error) && Changed) Changed();
				}
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && SettingsId == id) FinishSettings();
		};
		auto snap = CurrentSnap();
		bool show = snap.ShowGrid;
		commit(ImGui::Checkbox("Show grid", &show), [&](auto &p) { p.PreviewGrid.Show = show; });
		ImGui::SameLine();
		bool snapping = CurrentSnap().SnapGrid;
		commit(ImGui::Checkbox("Snap", &snapping), [&](auto &p) { p.PreviewGrid.Snap = snapping; });
		double grid[]{CurrentSnap().GridSize.X, CurrentSnap().GridSize.Y};
		commit(ImGui::InputScalarN("Grid size", ImGuiDataType_Double, grid, 2), [&](auto &p) {
			p.PreviewGrid.Size = {grid[0], grid[1]};
		});
		bool rulers = CurrentSnap().ShowRulers;
		commit(ImGui::Checkbox("Show guides", &rulers), [&](auto &p) { p.ShowPreviewRulers = rulers; });
		const size_t count = CurrentSnap().Guides.size();
		for (size_t i = 0; i < count; ++i) {
			ImGui::PushID(static_cast<int>(i));
			const auto guide = CurrentSnap().Guides[i];
			bool vertical = guide.Axis == engine::imagegraph::PreviewRulerAxis::Vertical;
			commit(ImGui::Checkbox("Vertical", &vertical), [&](auto &p) {
				p.PreviewRulers[i].Axis = vertical ? engine::imagegraph::PreviewRulerAxis::Vertical
												   : engine::imagegraph::PreviewRulerAxis::Horizontal;
			});
			ImGui::SameLine();
			double position = guide.Position;
			commit(ImGui::InputScalar("Position", ImGuiDataType_Double, &position), [&](auto &p) {
				p.PreviewRulers[i].Position = position;
			});
			const bool remove = ImGui::SmallButton("Remove");
			commit(remove, [&](auto &p) {
				p.PreviewRulers.erase(p.PreviewRulers.begin() + static_cast<ptrdiff_t>(i));
			});
			ImGui::PopID();
			if (remove) {
				FinishSettings();
				break;
			}
		}
		if (CurrentSnap().Guides.size() < engine::imagegraph::Limits::MaximumArrayElements) {
			const bool add = ImGui::SmallButton("Add guide");
			commit(add, [](auto &p) { p.PreviewRulers.push_back({}); });
			if (add) FinishSettings();
		}
		const bool reset = ImGui::SmallButton("Reset grid / guides");
		commit(reset, [](auto &p) {
			p.PreviewGrid = {};
			p.PreviewRulers.clear();
			p.ShowPreviewRulers = false;
		});
		if (reset) FinishSettings();
		ImGui::TreePop();
	}

	void Vector2Panel::DrawOverlay(
		engine::render::Renderer &renderer, Vector2EditorRect image, double scale, std::string_view selected
	) {
		if (!Document || !std::isfinite(scale) || scale <= 0) return;
		const auto Snap = CurrentSnap();
		auto *draw = ImGui::GetWindowDrawList();
		draw->PushClipRect(
			{static_cast<float>(image.X), static_cast<float>(image.Y)},
			{static_cast<float>(image.X + image.Width), static_cast<float>(image.Y + image.Height)},
			true
		);
		if (Snap.ShowGrid) {
			const auto grid = [&](double step, double extent, bool vertical) {
				if (step == 0) return;
				if (!std::isfinite(step) || step < 0 ||
					extent / step > engine::imagegraph::Limits::MaximumArrayElements) {
					Error = {
						Status::LimitExceeded, {}, "preview_grid", "grid exceeds the preview drawing budget"
					};
					return;
				}
				for (double offset = step; offset < extent; offset += step) {
					if (vertical)
						draw->AddLine(
							Point({image.X + offset, image.Y}),
							Point({image.X + offset, image.Y + image.Height}),
							0x80FFFFFF
						);
					else
						draw->AddLine(
							Point({image.X, image.Y + offset}),
							Point({image.X + image.Width, image.Y + offset}),
							0x80FFFFFF
						);
				}
			};
			grid(Snap.GridSize.X * scale, image.Width, true);
			grid(Snap.GridSize.Y * scale, image.Height, false);
		}

		if (Snap.ShowRulers)
			for (const auto &guide : Snap.Guides) {
				if (guide.Axis == engine::imagegraph::PreviewRulerAxis::Vertical && guide.Position >= 0 &&
					guide.Position * scale <= image.Width)
					draw->AddLine(
						Point({image.X + guide.Position * scale, image.Y}),
						Point({image.X + guide.Position * scale, image.Y + image.Height}),
						0x80FFFFFF
					);
				else if (guide.Axis == engine::imagegraph::PreviewRulerAxis::Horizontal &&
						 guide.Position >= 0 && guide.Position * scale <= image.Height)
					draw->AddLine(
						Point({image.X, image.Y + guide.Position * scale}),
						Point({image.X + image.Width, image.Y + guide.Position * scale}),
						0x80FFFFFF
					);
			}
		const auto &io = ImGui::GetIO();
		bool claimed = false;
		for (const auto &node : Document->Nodes) {
			const auto found = Rows.find(node.Id);
			if (found == Rows.end() || !found->second.Valid) continue;
			auto &row = found->second;
			const auto &v = row.Value;
			if (v.ProcessorCount > 1 || (selected.empty() ? !v.ShowOnGlobal : selected != node.Id)) continue;
			Vector2 factor =
				v.RelativeUnit ? Vector2{double(v.ProjectWidth), double(v.ProjectHeight)} : Vector2{1, 1};
			Vector2 position;
			if (!Vector2OverlayPosition({v.X, v.Y}, factor, v.Offset, {image.X, image.Y}, scale, position))
				continue;
			const bool hovering = ImGui::IsWindowHovered() && !claimed && io.MousePos.x >= image.X &&
								  io.MousePos.x <= image.X + image.Width && io.MousePos.y >= image.Y &&
								  io.MousePos.y <= image.Y + image.Height &&
								  HitVector2Overlay(
									  {io.MousePos.x, io.MousePos.y},
									  position,
									  v.Style,
									  v.Size,
									  v.Scale * engine::ui::Scaled(1),
									  scale
								  );
			claimed = claimed || hovering;
			if (hovering && GestureNode.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				Begin(node.Id, true);
				DragMouse = {io.MousePos.x, io.MousePos.y};
				DragValue = {v.X, v.Y};
			}
			if (OverlayDragging && GestureNode == node.Id) {
				Vector2 value;
				if (Vector2OverlayDrag(
						DragValue,
						{io.MousePos.x - DragMouse.X, io.MousePos.y - DragMouse.Y},
						factor,
						scale,
						Snap,
						io.KeyCtrl,
						value
					))
					Edit(node.Id, value);
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
					!ImGui::IsMouseDown(ImGuiMouseButton_Left))
					End();
			}
			const uint32_t colour = hovering ? 0xFFFFFFFF : 0xCCFFFFFF;
			if (v.Style == 0) {
				const float radius = static_cast<float>(std::max(0., engine::ui::Scaled(8) * v.Scale));
				draw->AddCircle(Point(position), radius, colour, 32);
				draw->AddCircleFilled(Point(position), std::max(1.f, radius * .25f), colour);
			} else {
				const Vector2 half{v.Size.X * scale * .5, v.Size.Y * scale * .5};
				const auto min = Point({position.X - half.X, position.Y - half.Y});
				const auto max = Point({position.X + half.X, position.Y + half.Y});
				if (v.Style == 1 && v.Shape == 0)
					draw->AddRect(min, max, colour);
				else if (v.Style == 1 && v.Shape == 1)
					draw->AddEllipse(Point(position), Point(half), colour, 0, 32);
				else if (v.Style == 2 && Upload(row, node.Id, renderer)) {
					if (void *handle = renderer.TextureHandle(row.Texture))
						draw->AddImage(
							reinterpret_cast<ImTextureID>(handle),
							min,
							max,
							{0, 0},
							{1, 1},
							hovering ? 0xFFFFFFFF : 0x80FFFFFF
						);
				}
			}
		}
		draw->PopClipRect();
		if (OverlayDragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) End();
	}
	void Vector2Panel::Close(engine::render::Renderer &renderer) {
		End();
		FinishSettings();
		for (const auto &item : PendingDrop)
			renderer.DropTexture(item.first);
		PendingDrop.clear();
		for (auto &[id, row] : Rows) {
			(void)id;
			if (row.Texture.IsValid()) renderer.DropTexture(row.Texture);
			if (row.Retired.IsValid()) renderer.DropTexture(row.Retired);
		}
		Rows.clear();
	}
}
