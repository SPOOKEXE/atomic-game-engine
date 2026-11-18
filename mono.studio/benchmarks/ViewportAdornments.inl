// The uncached drawer is the output oracle while the real editor caller is timed.
namespace {
	ImU32 AdornmentColour(const core::Color3 &colour, float transparency) {
		return ImGui::GetColorU32(
			ImVec4(colour.R, colour.G, colour.B, std::clamp(1 - transparency, 0.0f, 1.0f))
		);
	}
	void ReferenceAdornments(
		const render::AdornmentGeometry &geometry, const studio::PanelProjection &panel, ImDrawList &list
	) {
		size_t faces = 0;
		for (const auto &face : geometry.Faces()) {
			if (faces++ >= 4000) break;
			ImVec2 points[4];
			bool whole = true;
			for (size_t corner = 0; corner < 4 && whole; ++corner) {
				glm::vec2 projected;
				whole = panel.WorldToPanel(face.Corners[corner], projected);
				points[corner] = {projected.x, projected.y};
			}
			if (whole) list.AddConvexPolyFilled(points, 4, AdornmentColour(face.Colour, face.Transparency));
		}
		size_t attempted = 0;
		for (const auto &line : geometry.Lines()) {
			if (attempted++ >= 20000) break;
			glm::vec2 from, to;
			if (!panel.ProjectSegment(line.From, line.To, from, to)) continue;
			if ((from.x < 60 && to.x < 60) || (from.x > 860 && to.x > 860) || (from.y < 25 && to.y < 25) ||
				(from.y > 425 && to.y > 425))
				continue;
			float width = 1;
			if (line.Thickness > 0) {
				const auto middle = (line.From + line.To) * .5f;
				auto sideways = (line.To - line.From).Cross(panel.Eye - middle);
				const float length = sideways.Magnitude();
				if (length > 0) {
					sideways = sideways * (line.Thickness / length);
					glm::vec2 here, beside;
					if (panel.WorldToPanel(middle, here) && panel.WorldToPanel(middle + sideways, beside))
						width = std::clamp(std::hypot(beside.x - here.x, beside.y - here.y), 1.0f, 12.0f);
				}
			}
			list.AddLine(
				{from.x, from.y}, {to.x, to.y}, AdornmentColour(line.Colour, line.Transparency), width
			);
		}
	}
	void VerifyAdornmentOutput(
		const ImDrawList &actual,
		const ImDrawList &expected,
		const char *row,
		size_t count,
		size_t frameNumber
	) {
		const auto requireSame = [&](bool okay, const char *reason) {
			if (okay) return;
			throw std::runtime_error(
				std::string(reason) + " row=" + row + " adornments=" + std::to_string(count) + " frame=" +
				std::to_string(frameNumber) + " actual_vertices=" + std::to_string(actual.VtxBuffer.Size) +
				" expected_vertices=" + std::to_string(expected.VtxBuffer.Size) +
				" actual_indices=" + std::to_string(actual.IdxBuffer.Size) +
				" expected_indices=" + std::to_string(expected.IdxBuffer.Size) +
				" actual_commands=" + std::to_string(actual.CmdBuffer.Size) +
				" expected_commands=" + std::to_string(expected.CmdBuffer.Size) +
				" actual_fringe=" + std::to_string(actual._FringeScale) +
				" expected_fringe=" + std::to_string(expected._FringeScale)
			);
		};
		requireSame(
			actual.VtxBuffer.Size == expected.VtxBuffer.Size &&
				actual.IdxBuffer.Size == expected.IdxBuffer.Size,
			"actual adornment vertex/index counts differ from uncached oracle"
		);
		requireSame(
			actual.CmdBuffer.Size == expected.CmdBuffer.Size, "actual adornment draw command count differs"
		);
		for (int index = 0; index < actual.CmdBuffer.Size; ++index) {
			const auto &a = actual.CmdBuffer[index];
			const auto &b = expected.CmdBuffer[index];
			requireSame(
				a.ElemCount == b.ElemCount && a.IdxOffset == b.IdxOffset && a.VtxOffset == b.VtxOffset &&
					a.ClipRect.x == b.ClipRect.x && a.ClipRect.y == b.ClipRect.y &&
					a.ClipRect.z == b.ClipRect.z && a.ClipRect.w == b.ClipRect.w &&
					a.TexRef._TexData == b.TexRef._TexData && a.TexRef._TexID == b.TexRef._TexID &&
					a.UserCallback == b.UserCallback,
				"actual adornment draw command differs from uncached oracle"
			);
		}
		for (int index = 0; index < actual.VtxBuffer.Size; ++index) {
			const auto &a = actual.VtxBuffer[index];
			const auto &b = expected.VtxBuffer[index];
			requireSame(
				a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y &&
					a.col == b.col,
				"actual adornment vertex differs from uncached oracle"
			);
		}
		for (int index = 0; index < actual.IdxBuffer.Size; ++index)
			requireSame(
				actual.IdxBuffer[index] == expected.IdxBuffer[index],
				"actual adornment topology differs from uncached oracle"
			);
	}
	void AdornmentDrawing(size_t count, int mode, size_t trials, size_t warmups, bool memory) {
		Fixture fixture(0, false);
		ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
		gui::RegisterGuiClasses();
		Probe::Enter(fixture.Editor, [&](ecs::Store &store) {
			const auto workspace = scene::WorkspaceOf(store);
			for (size_t index = 0; index < count; ++index) {
				const auto part = store.CreateInstance(scene::PartClass(), "Adornee");
				store.SetParent(part, workspace);
				const core::CFrame frame(
					{(mode == 3 ? 1000.0f : -12.0f) + float(index % 16) * 1.5f,
					 -6 + float((index / 16) % 8) * 1.5f,
					 -25 - float(index / 128) * 3}
				);
				store.Set(part, scene::Transform{frame});
				store.Set(part, scene::PreviousTransform{frame});
				store.Set(part, scene::Bounds{{.5f, .5f, .5f}});
				fixture.Members.push_back(part);
				fixture.Before.push_back(frame);
				const auto adornment = store.CreateInstance(
					gui::GuiClass(mode == 1 ? "SphereHandleAdornment" : "SelectionBox"), "Adornment"
				);
				store.SetParent(adornment, workspace);
				auto style = *store.Get<gui::Adornment>(adornment);
				style.Adornee = part;
				style.ZIndex = static_cast<int32_t>(index % 3);
				style.Color = {float(index % 3) * .25f, .65f, 1};
				store.Set(adornment, style);
				if (mode == 2) {
					auto outline = *store.Get<gui::SelectionOutline>(adornment);
					outline.SurfaceTransparency = .6f;
					store.Set(adornment, outline);
				}
			}
		});
		fixture.Drawables = count;
		const auto panel = Panel();
		render::AdornmentGeometry geometry;
		ImDrawList expected(ImGui::GetDrawListSharedData());
		size_t frameNumber = 0;
		const char *row = mode == 0	  ? "actual_adornments_static_boxes"
						  : mode == 1 ? "actual_adornments_curved_handles"
						  : mode == 2 ? "actual_adornments_filled_faces"
						  : mode == 3 ? "actual_adornments_offscreen"
									  : "actual_adornments_moving";
		Measure(
			fixture,
			row,
			1,
			trials,
			warmups,
			memory,
			false,
			[&] {
				fixture.Begin();
				Probe::Enter(fixture.Editor, [&](ecs::Store &store) {
					if (mode == 4) {
						for (size_t index = 0; index < count; ++index) {
							auto frame = fixture.Before[index];
							frame.Position.X += float(frameNumber % 8) * .1f;
							store.Set(fixture.Members[index], scene::Transform{frame});
						}
					}
					store.SetFrame(1.0f / 60, float(frameNumber++ % 4) / 3);
					geometry.Build(store);
				});
				Require(
					geometry.Lines().size() == count * (mode == 1 ? 96 : 12),
					"adornment fixture line count differs"
				);
				Require(
					geometry.Faces().size() == (mode == 2 ? count * 6 : 0),
					"adornment fixture face count differs"
				);
				expected._ResetForNewFrame();
				// Match the foreground list's pixel density before ImGui chooses its line tessellation path.
				expected._SetPixelDensity(ImGui::GetForegroundDrawList()->_InvFringeScale);
				expected.Flags = ImGui::GetForegroundDrawList()->Flags;
				expected.PushTexture(ImGui::GetIO().Fonts->TexRef);
				expected.PushClipRectFullScreen();
				ReferenceAdornments(geometry, panel, expected);
			},
			[&] { Probe::Adornments(fixture.Editor, panel); },
			[&] {
				VerifyAdornmentOutput(*ImGui::GetForegroundDrawList(), expected, row, count, frameNumber);
				Require(
					(expected.IdxBuffer.Size > 0) == (count > 0 && mode != 3),
					"adornment visible/offscreen oracle differs"
				);
				return Findings{};
			}
		);
		std::cout
			<< "viewport-adornments row=" << row << " adornments=" << count
			<< " resolved_lines=" << geometry.Lines().size() << " resolved_faces=" << geometry.Faces().size()
			<< " oracle=all_vertices_uv_colours_indices uncached_oracle=excluded"
			<< " frame_setup_mutation=excluded actual_DrawAdornments=included caller_calls_per_sample=1\n";
	}
	void RunAdornments() {
		if (!Flag("MONO_VIEWPORT_ADORNMENTS_BENCH")) {
			std::cout << "viewport-adornments skipped=opt_in_required\n";
			return;
		}
		const size_t trials = Option("MONO_VIEWPORT_ADORNMENTS_TRIALS", 101, 1, CAPACITY);
		const size_t warmups = Option("MONO_VIEWPORT_ADORNMENTS_WARMUPS", 3, 0, 32);
		const bool memory = Flag("MONO_VIEWPORT_ADORNMENTS_MEMORY");
		Latest.clear();
		Latest.reserve(15);
		studio::SetConfigRoot(core::Paths::Base() / "studio-adornment-benchmark-config");
		for (size_t count : {0u, 64u, 512u})
			for (int mode = 0; mode < 5; ++mode)
				AdornmentDrawing(count, mode, trials, warmups, memory);
		Print();
		Latest.clear();
	}
}

BENCH("Viewport adornments | actual drawing with full uncached output oracle", 1) {
	RunAdornments();
}
