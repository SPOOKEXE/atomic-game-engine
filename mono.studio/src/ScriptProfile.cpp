// Which script is spending the tick.
//
// **Two measurements, two costs.** `RuntimeLimits::StepBudget` bounds an
// interrupt counter, so the top-level script cost is available without a
// profiler. Opening this panel also opts the runtime into source sampling at
// loop back-edges, calls and returns. The latter is intentionally diagnostic
// work and stops when the panel closes.
//
// **Steps and not milliseconds**, which is `script/AGENTS.md`'s rule rather than
// a limitation: a wall-clock figure makes what a script cost depend on how busy
// the machine was, so two runs of one recording would disagree about it. A step
// is the same on every machine, which is what makes two numbers here comparable
// at all.
//
// ## What this measures, said plainly
//
// A script's **top level** - the run that `RunWorldScripts` performs when the
// world starts. Not its heartbeat work. `SignalTable` holds connections as
// opaque callables and nothing records which script made one, so attributing
// beat time to a script would be a guess with a number's confidence. The panel
// says so rather than letting a reader assume otherwise.

#include <engine/core/FrameGraph.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <imgui.h>
#include <numeric>
#include <string>
#include <string_view>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::core::FrameGraph;
	using engine::core::FrameSpan;
	using engine::ecs::Store;
	using engine::script::ScriptCost;
	using engine::script::ScriptProfileNode;
	using engine::world::WorldId;

	void Editor::DrawScriptProfile() {
		if (!ShowScriptProfile) {
			return;
		}

		if (!ImGui::Begin("Script Profiler", &ShowScriptProfile)) {
			ImGui::End();
			return;
		}

		if (Runs.empty()) {
			// **Not an empty table.** "No scripts are running" and "the scripts
			// that are running cost nothing" are different answers and a blank
			// grid gives the second when it means the first.
			ImGui::TextDisabled("nothing is running - press Play or Run");
			ImGui::End();
			return;
		}

		ImGui::TextDisabled("VM steps in each script's top level, not its heartbeat work");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint(
			"##script-filter", "filter scripts", ScriptProfileFilter.data(), ScriptProfileFilter.size()
		);
		ImGui::Separator();

		for (const WorldRun &run : Runs) {
			if (run.Runtime == nullptr) {
				continue;
			}

			const std::string world(Label(Universe->NameOf(run.World)));

			// Copied out of the runtime before anything is drawn, and sorted
			// here rather than in the runtime: the order a panel wants is a
			// presentation decision and `Costs()` promises run order, which is
			// what a recording depends on.
			std::vector<ScriptCost> costs(run.Runtime->Costs().begin(), run.Runtime->Costs().end());
			std::sort(costs.begin(), costs.end(), [](const ScriptCost &left, const ScriptCost &right) {
				return left.Steps > right.Steps;
			});

			uint64_t total = 0;
			for (const ScriptCost &cost : costs) {
				total += cost.Steps;
			}

			char header[192];
			std::snprintf(
				header,
				sizeof(header),
				"%s - %zu script%s, %llu steps###%u",
				world.c_str(),
				costs.size(),
				costs.size() == 1 ? "" : "s",
				static_cast<unsigned long long>(total),
				run.World.Index
			);

			if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
				continue;
			}

			if (costs.empty()) {
				ImGui::TextDisabled("no scripts ran in this world");
				continue;
			}

			// Names are read out of the store, which means entering the world.
			// Gathered first and drawn after, like every other panel here.
			std::vector<std::string> names(costs.size());
			Universe->Enter(run.World, [&](Store &store) {
				for (size_t index = 0; index < costs.size(); index++) {
					names[index] = store.Alive(costs[index].Instance)
									   ? std::string(Label(store.InstanceNameOf(costs[index].Instance)))
									   : std::string("(deleted)");
				}
			});

			ImGui::PushID(static_cast<int>(run.World.Index));
			if (ImGui::BeginTable("##costs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("script", ImGuiTableColumnFlags_WidthStretch, 0.5f);
				ImGui::TableSetupColumn("steps", ImGuiTableColumnFlags_WidthStretch, 0.25f);
				ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthStretch, 0.25f);
				ImGui::TableHeadersRow();

				for (size_t index = 0; index < costs.size(); index++) {
					const ScriptCost &cost = costs[index];
					if (ScriptProfileFilter[0] != '\0' &&
						names[index].find(ScriptProfileFilter.data()) == std::string::npos) {
						continue;
					}

					ImGui::TableNextRow();
					ImGui::TableNextColumn();

					if (cost.Completed) {
						ImGui::TextUnformatted(names[index].c_str());
					} else {
						// **A script that raised has a step count and it is not
						// comparable to one that finished** - it stopped where
						// it stopped. Marked rather than silently ranked beside
						// the others.
						ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", names[index].c_str());
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip("raised part way through - its count is where it stopped");
						}
					}

					ImGui::TableNextColumn();
					ImGui::Text("%llu", static_cast<unsigned long long>(cost.Steps));

					ImGui::TableNextColumn();
					if (total == 0) {
						// Every division in this panel is guarded, and this is
						// the one that fires: a world whose scripts are all
						// trivial legitimately totals zero steps.
						ImGui::TextDisabled("-");
					} else {
						const float share = static_cast<float>(cost.Steps) / static_cast<float>(total);

						char label[32];
						std::snprintf(label, sizeof(label), "%.0f%%", static_cast<double>(share) * 100.0);
						ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), label);
					}
				}
				ImGui::EndTable();
			}
			ImGui::PopID();
		}

		ImGui::Separator();
		ImGui::TextDisabled("live source samples while this panel is open");
		ImGui::Checkbox("hierarchy", &ScriptProfileHierarchy);

		for (WorldRun &run : Runs) {
			if (run.Runtime == nullptr) {
				continue;
			}

			engine::script::ScriptProfiler &profile = run.Runtime->Profile();
			const std::span<const ScriptProfileNode> nodes = profile.Nodes();
			const std::string world(Label(Universe->NameOf(run.World)));
			ImGui::PushID(static_cast<int>(run.World.Index));
			if (!ImGui::CollapsingHeader(
					(world + " source profile").c_str(), ImGuiTreeNodeFlags_DefaultOpen
				)) {
				ImGui::PopID();
				continue;
			}

			if (ImGui::SmallButton("clear samples")) {
				profile.Clear();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%zu nodes", nodes.size());
			if (profile.DroppedNodes() != 0) {
				ImGui::SameLine();
				ImGui::TextColored(
					ImVec4(0.9f, 0.55f, 0.25f, 1.0f), "%zu source nodes dropped", profile.DroppedNodes()
				);
			}

			std::vector<std::string_view> sources;
			std::vector<std::vector<size_t>> children(nodes.size());
			std::vector<size_t> roots;
			for (size_t index = 0; index < nodes.size(); index++) {
				const ScriptProfileNode &node = nodes[index];
				if (std::find(sources.begin(), sources.end(), node.Source) == sources.end()) {
					sources.push_back(node.Source);
				}
				if (node.Parent == UINT32_MAX) {
					roots.push_back(index);
				} else if (node.Parent < nodes.size()) {
					children[node.Parent].push_back(index);
				}
			}
			std::sort(sources.begin(), sources.end());
			const std::string_view selectedSource(ScriptProfileSource.data());
			if (ImGui::BeginCombo(
					"source", selectedSource.empty() ? "all sources" : ScriptProfileSource.data()
				)) {
				if (ImGui::Selectable("all sources", selectedSource.empty())) {
					ScriptProfileSource[0] = '\0';
				}
				for (const std::string_view source : sources) {
					const bool selected = source == selectedSource;
					if (ImGui::Selectable(source.data(), selected)) {
						std::snprintf(
							ScriptProfileSource.data(),
							ScriptProfileSource.size(),
							"%.*s",
							static_cast<int>(source.size()),
							source.data()
						);
					}
				}
				ImGui::EndCombo();
			}
			const std::string_view sourceFilter(ScriptProfileSource.data());

			const auto matches = [&](const ScriptProfileNode &node) {
				if (!sourceFilter.empty() && node.Source != sourceFilter) {
					return false;
				}
				return ScriptProfileFilter[0] == '\0' ||
					   node.Source.find(ScriptProfileFilter.data()) != std::string::npos ||
					   node.Function.find(ScriptProfileFilter.data()) != std::string::npos;
			};
			const auto openFolds = [this](std::string_view source) {
				std::snprintf(
					ScriptProfileSource.data(),
					ScriptProfileSource.size(),
					"%.*s",
					static_cast<int>(source.size()),
					source.data()
				);
				ShowScriptFolds = true;
			};
			if (ScriptProfileSource[0] != '\0' && ImGui::SmallButton("open script folds")) {
				openFolds(ScriptProfileSource.data());
			}

			if (nodes.empty()) {
				ImGui::TextDisabled("no source samples yet");
			} else {
				std::vector<uint64_t> visibleNanoseconds(nodes.size());
				std::function<uint64_t(size_t)> sumVisible;
				sumVisible = [&](size_t index) {
					uint64_t total = matches(nodes[index]) ? nodes[index].SelfNanoseconds : 0;
					for (const size_t child : children[index]) {
						total += sumVisible(child);
					}
					visibleNanoseconds[index] = total;
					return total;
				};
				uint64_t flameTotal = 0;
				for (const size_t root : roots) {
					flameTotal += sumVisible(root);
				}

				if (ImGui::TreeNodeEx("source flame graph", ImGuiTreeNodeFlags_DefaultOpen)) {
					if (flameTotal == 0) {
						ImGui::TextDisabled("the current filter has no timed samples");
					} else {
						const float rowHeight = std::max(engine::ui::Scaled(16.0f), 1.0f);
						const float graphWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
						const ImVec2 origin = ImGui::GetCursorScreenPos();
						ImDrawList *draw = ImGui::GetWindowDrawList();
						const ScriptProfileNode *hovered = nullptr;
						uint32_t rows = 1;

						std::function<void(size_t, float, float, uint32_t)> drawFlame;
						drawFlame = [&](size_t index, float left, float width, uint32_t depth) {
							if (visibleNanoseconds[index] == 0) {
								return;
							}
							const ScriptProfileNode &node = nodes[index];
							rows = std::max(rows, depth + 1);
							const ImVec2 upper(left, origin.y + static_cast<float>(depth) * rowHeight);
							const ImVec2 lower(left + width, upper.y + rowHeight);
							draw->AddRectFilled(upper, lower, engine::ui::AccentColour());
							if (ImGui::IsMouseHoveringRect(upper, lower)) {
								hovered = &node;
								draw->AddRect(upper, lower, engine::ui::BrightColour());
							}
							if (width > engine::ui::Scaled(42.0f)) {
								const std::string_view label =
									node.Function.empty() ? node.Source : std::string_view(node.Function);
								draw->PushClipRect(upper, lower, true);
								draw->AddText(
									ImVec2(
										upper.x + engine::ui::Scaled(3.0f), upper.y + engine::ui::Scaled(1.0f)
									),
									IM_COL32(0, 0, 0, 255),
									label.data(),
									label.data() + label.size()
								);
								draw->PopClipRect();
							}

							float childLeft = left;
							for (const size_t child : children[index]) {
								const float childWidth = width *
														 static_cast<float>(visibleNanoseconds[child]) /
														 static_cast<float>(visibleNanoseconds[index]);
								drawFlame(child, childLeft, childWidth, depth + 1);
								childLeft += childWidth;
							}
						};

						float left = origin.x;
						for (const size_t root : roots) {
							const float width = graphWidth * static_cast<float>(visibleNanoseconds[root]) /
												static_cast<float>(flameTotal);
							drawFlame(root, left, width, 0);
							left += width;
						}
						ImGui::Dummy(ImVec2(graphWidth, static_cast<float>(rows) * rowHeight));
						if (hovered != nullptr) {
							ImGui::BeginTooltip();
							ImGui::TextUnformatted(hovered->Function.c_str());
							ImGui::TextUnformatted(hovered->Source.c_str());
							ImGui::Text(
								"line %d, %.3f ms self",
								hovered->Line,
								static_cast<double>(hovered->SelfNanoseconds) / 1'000'000.0
							);
							ImGui::Text(
								"%llu B, %llu yields, %llu samples",
								static_cast<unsigned long long>(hovered->AllocatedBytes),
								static_cast<unsigned long long>(hovered->Yields),
								static_cast<unsigned long long>(hovered->Samples)
							);
							ImGui::EndTooltip();
						}
					}
					ImGui::TreePop();
				}

				if (ScriptProfileHierarchy) {
					std::function<bool(size_t)> branchMatches;
					branchMatches = [&](size_t index) {
						if (matches(nodes[index])) {
							return true;
						}
						for (const size_t child : children[index]) {
							if (branchMatches(child)) {
								return true;
							}
						}
						return false;
					};
					std::function<void(size_t)> drawNode;
					drawNode = [&](size_t index) {
						const ScriptProfileNode &node = nodes[index];
						if (!branchMatches(index)) {
							return;
						}
						const bool hasChildren = !children[index].empty();
						const ImGuiTreeNodeFlags flags = hasChildren ? 0 : ImGuiTreeNodeFlags_Leaf;
						const bool open = ImGui::TreeNodeEx(
							static_cast<void *>(const_cast<ScriptProfileNode *>(&node)),
							flags,
							"%s%s%d",
							node.Function.c_str(),
							node.Line == 0 ? "" : ":",
							node.Line
						);
						ImGui::SameLine();
						ImGui::TextDisabled(
							"%.3f ms  %llu B  %llu yields",
							static_cast<double>(node.SelfNanoseconds) / 1'000'000.0,
							static_cast<unsigned long long>(node.AllocatedBytes),
							static_cast<unsigned long long>(node.Yields)
						);
						ImGui::SameLine();
						ImGui::PushID(static_cast<int>(index));
						if (ImGui::SmallButton("folds")) {
							openFolds(node.Source);
						}
						ImGui::PopID();
						if (!open) {
							return;
						}
						for (const size_t child : children[index]) {
							drawNode(child);
						}
						if (!node.Bindings.empty()) {
							ImGui::PushID(static_cast<int>(index));
							if (ImGui::TreeNodeEx("native bindings", ImGuiTreeNodeFlags_DefaultOpen)) {
								for (const ScriptProfileNode::Binding &binding : node.Bindings) {
									ImGui::TextUnformatted(binding.Name.c_str());
									ImGui::SameLine();
									const float share = node.SelfNanoseconds == 0
															? 0.0f
															: static_cast<float>(binding.Nanoseconds) /
																  static_cast<float>(node.SelfNanoseconds);
									char label[64];
									std::snprintf(
										label,
										sizeof(label),
										"%.3f ms, %llu yielded",
										static_cast<double>(binding.Nanoseconds) / 1'000'000.0,
										static_cast<unsigned long long>(binding.Yields)
									);
									ImGui::ProgressBar(
										std::clamp(share, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), label
									);
								}
								ImGui::TreePop();
							}
							ImGui::PopID();
						}
						ImGui::TreePop();
					};
					for (const size_t root : roots) {
						drawNode(root);
					}
				} else {
					std::vector<size_t> rows;
					for (size_t index = 0; index < nodes.size(); index++) {
						if (matches(nodes[index])) {
							rows.push_back(index);
						}
					}

					if (ImGui::BeginTable(
							"##source-samples",
							7,
							ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
								ImGuiTableFlags_Sortable
						)) {
						ImGui::TableSetupColumn("function", ImGuiTableColumnFlags_WidthStretch, 0.25f);
						ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthStretch, 0.2f);
						ImGui::TableSetupColumn("line", ImGuiTableColumnFlags_WidthStretch, 0.08f);
						ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthStretch, 0.12f);
						ImGui::TableSetupColumn("bytes", ImGuiTableColumnFlags_WidthStretch, 0.12f);
						ImGui::TableSetupColumn("yields", ImGuiTableColumnFlags_WidthStretch, 0.1f);
						ImGui::TableSetupColumn("samples", ImGuiTableColumnFlags_WidthStretch, 0.13f);
						ImGui::TableHeadersRow();

						if (ImGuiTableSortSpecs *sort = ImGui::TableGetSortSpecs();
							sort != nullptr && sort->SpecsCount != 0) {
							const ImGuiTableColumnSortSpecs &spec = sort->Specs[0];
							const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
							std::sort(rows.begin(), rows.end(), [&](size_t left, size_t right) {
								const ScriptProfileNode &a = nodes[left];
								const ScriptProfileNode &b = nodes[right];
								bool leftFirst = false;
								bool rightFirst = false;
								switch (spec.ColumnIndex) {
								case 0:
									leftFirst = a.Function < b.Function;
									rightFirst = b.Function < a.Function;
									break;
								case 1:
									leftFirst = a.Source < b.Source;
									rightFirst = b.Source < a.Source;
									break;
								case 2:
									leftFirst = a.Line < b.Line;
									rightFirst = b.Line < a.Line;
									break;
								case 3:
									leftFirst = a.SelfNanoseconds < b.SelfNanoseconds;
									rightFirst = b.SelfNanoseconds < a.SelfNanoseconds;
									break;
								case 4:
									leftFirst = a.AllocatedBytes < b.AllocatedBytes;
									rightFirst = b.AllocatedBytes < a.AllocatedBytes;
									break;
								case 5:
									leftFirst = a.Yields < b.Yields;
									rightFirst = b.Yields < a.Yields;
									break;
								default:
									leftFirst = a.Samples < b.Samples;
									rightFirst = b.Samples < a.Samples;
									break;
								}
								if (leftFirst == rightFirst) {
									return left < right;
								}
								return ascending ? leftFirst : rightFirst;
							});
							sort->SpecsDirty = false;
						}

						for (size_t index : rows) {
							const ScriptProfileNode &node = nodes[index];
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(node.Function.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(node.Source.c_str());
							ImGui::TableNextColumn();
							ImGui::Text("%d", node.Line);
							ImGui::TableNextColumn();
							ImGui::Text("%.3f", static_cast<double>(node.SelfNanoseconds) / 1'000'000.0);
							ImGui::TableNextColumn();
							ImGui::Text("%llu", static_cast<unsigned long long>(node.AllocatedBytes));
							ImGui::TableNextColumn();
							ImGui::Text("%llu", static_cast<unsigned long long>(node.Yields));
							ImGui::TableNextColumn();
							ImGui::Text("%llu", static_cast<unsigned long long>(node.Samples));
						}
						ImGui::EndTable();
					}
				}
			}
			ImGui::PopID();
		}

		ImGui::End();
		DrawScriptFolds();
	}

	void Editor::DrawScriptFolds() {
		if (!ShowScriptFolds) {
			return;
		}

		if (!ImGui::Begin("Script Folds Profiler", &ShowScriptFolds)) {
			ImGui::End();
			return;
		}

		const std::string_view source(ScriptProfileSource.data());
		if (source.empty()) {
			ImGui::TextDisabled("select a source in Script Profiler first");
			ImGui::End();
			return;
		}

		struct Fold {
			std::string_view Function;
			int Line = 0;
			uint64_t Nanoseconds = 0;
			uint64_t Bytes = 0;
			uint64_t Yields = 0;
			uint64_t Samples = 0;
		};
		std::vector<Fold> folds;
		for (const WorldRun &run : Runs) {
			if (run.Runtime == nullptr) {
				continue;
			}
			for (const ScriptProfileNode &node : run.Runtime->Profile().Nodes()) {
				if (node.Source != source || node.Line == 0) {
					continue;
				}
				auto found = std::find_if(folds.begin(), folds.end(), [&node](const Fold &fold) {
					return fold.Function == node.Function && fold.Line == node.Line;
				});
				if (found == folds.end()) {
					folds.push_back(
						Fold{
							node.Function,
							node.Line,
							node.SelfNanoseconds,
							node.AllocatedBytes,
							node.Yields,
							node.Samples,
						}
					);
				} else {
					found->Nanoseconds += node.SelfNanoseconds;
					found->Bytes += node.AllocatedBytes;
					found->Yields += node.Yields;
					found->Samples += node.Samples;
				}
			}
		}
		std::sort(folds.begin(), folds.end(), [](const Fold &left, const Fold &right) {
			return left.Nanoseconds > right.Nanoseconds;
		});

		ImGui::TextUnformatted(source.data(), source.data() + source.size());
		if (folds.empty()) {
			ImGui::TextDisabled("no sampled function or line folds yet");
			ImGui::End();
			return;
		}

		const uint64_t total =
			std::accumulate(folds.begin(), folds.end(), uint64_t{0}, [](uint64_t sum, const Fold &fold) {
				return sum + fold.Nanoseconds;
			});
		if (ImGui::BeginTable("##folds", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("function", ImGuiTableColumnFlags_WidthStretch, 0.25f);
			ImGui::TableSetupColumn("line", ImGuiTableColumnFlags_WidthStretch, 0.08f);
			ImGui::TableSetupColumn("self", ImGuiTableColumnFlags_WidthStretch, 0.25f);
			ImGui::TableSetupColumn("bytes", ImGuiTableColumnFlags_WidthStretch, 0.14f);
			ImGui::TableSetupColumn("yields", ImGuiTableColumnFlags_WidthStretch, 0.12f);
			ImGui::TableSetupColumn("samples", ImGuiTableColumnFlags_WidthStretch, 0.16f);
			ImGui::TableHeadersRow();

			for (const Fold &fold : folds) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(fold.Function.data(), fold.Function.data() + fold.Function.size());
				ImGui::TableNextColumn();
				ImGui::Text("%d", fold.Line);
				ImGui::TableNextColumn();
				const float share =
					total == 0 ? 0.0f : static_cast<float>(fold.Nanoseconds) / static_cast<float>(total);
				char label[48];
				std::snprintf(
					label, sizeof(label), "%.3f ms", static_cast<double>(fold.Nanoseconds) / 1'000'000.0
				);
				ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), label);
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(fold.Bytes));
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(fold.Yields));
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(fold.Samples));
			}
			ImGui::EndTable();
		}

		struct BindingUse {
			std::string_view Name;
			uint64_t Calls = 0;
			uint64_t Nanoseconds = 0;
			uint64_t Yields = 0;
		};
		std::vector<BindingUse> bindings;
		for (const WorldRun &run : Runs) {
			if (run.Runtime == nullptr) {
				continue;
			}
			for (const ScriptProfileNode &node : run.Runtime->Profile().Nodes()) {
				if (node.Source != source) {
					continue;
				}
				for (const ScriptProfileNode::Binding &binding : node.Bindings) {
					auto found =
						std::find_if(bindings.begin(), bindings.end(), [&binding](const BindingUse &use) {
							return use.Name == binding.Name;
						});
					if (found == bindings.end()) {
						bindings.push_back(
							BindingUse{
								binding.Name,
								binding.Calls,
								binding.Nanoseconds,
								binding.Yields,
							}
						);
					} else {
						found->Calls += binding.Calls;
						found->Nanoseconds += binding.Nanoseconds;
						found->Yields += binding.Yields;
					}
				}
			}
		}
		std::sort(bindings.begin(), bindings.end(), [](const BindingUse &left, const BindingUse &right) {
			return left.Nanoseconds > right.Nanoseconds;
		});
		const uint64_t bindingTotal = std::accumulate(
			bindings.begin(), bindings.end(), uint64_t{0}, [](uint64_t sum, const BindingUse &binding) {
				return sum + binding.Nanoseconds;
			}
		);

		ImGui::Separator();
		ImGui::TextDisabled("native bindings reached by this source");
		if (bindings.empty()) {
			ImGui::TextDisabled("no native binding calls sampled");
		} else if (ImGui::BeginTable(
					   "##fold-bindings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
				   )) {
			ImGui::TableSetupColumn("binding", ImGuiTableColumnFlags_WidthStretch, 0.35f);
			ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_WidthStretch, 0.12f);
			ImGui::TableSetupColumn("native time", ImGuiTableColumnFlags_WidthStretch, 0.38f);
			ImGui::TableSetupColumn("yielded", ImGuiTableColumnFlags_WidthStretch, 0.15f);
			ImGui::TableHeadersRow();

			for (const BindingUse &binding : bindings) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(binding.Name.data(), binding.Name.data() + binding.Name.size());
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(binding.Calls));
				ImGui::TableNextColumn();
				const float share = bindingTotal == 0 ? 0.0f
													  : static_cast<float>(binding.Nanoseconds) /
															static_cast<float>(bindingTotal);
				char label[48];
				std::snprintf(
					label, sizeof(label), "%.3f ms", static_cast<double>(binding.Nanoseconds) / 1'000'000.0
				);
				ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), label);
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(binding.Yields));
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}

	void Editor::DrawScripting() {
		if (!ShowScripting) {
			return;
		}

		if (!ImGui::Begin("Scripting", &ShowScripting)) {
			ImGui::End();
			return;
		}

		// This deliberately reads the frame graph instead of keeping a second
		// profiler record. A language adapter only has to submit Script spans for
		// its bindings to appear here, while the rest of the engine still has one
		// source of timing truth.
		const std::vector<FrameSpan> &frame = FrameGraph::Spans();
		std::vector<const FrameSpan *> spans;
		spans.reserve(frame.size());
		float first = 0.0f;
		float last = 0.0f;
		uint32_t shallowest = UINT32_MAX;
		for (const FrameSpan &span : frame) {
			if (!span.Name.starts_with("binding.")) {
				continue;
			}
			if (ScriptingFilter[0] != '\0' &&
				span.Name.find(ScriptingFilter.data()) == std::string_view::npos) {
				continue;
			}
			spans.push_back(&span);
			first = spans.size() == 1 ? span.StartMilliseconds : std::min(first, span.StartMilliseconds);
			last = std::max(last, span.StartMilliseconds + span.Milliseconds);
			shallowest = std::min(shallowest, span.Depth);
		}

		ImGui::TextDisabled("native binding work from the last completed frame");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint(
			"##binding-filter", "filter native bindings", ScriptingFilter.data(), ScriptingFilter.size()
		);
		if (spans.empty()) {
			ImGui::Separator();
			ImGui::TextDisabled("no scripting work recorded yet");
			ImGui::End();
			return;
		}

		const float widthMilliseconds = std::max(last - first, 0.001f);
		const float rowHeight = std::max(engine::ui::Scaled(16.0f), 1.0f);
		const float graphWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
		const float scale = graphWidth / widthMilliseconds;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const FrameSpan *hovered = nullptr;
		uint32_t rows = 1;

		for (const FrameSpan *span : spans) {
			const uint32_t depth = span->Depth >= shallowest ? span->Depth - shallowest : 0;
			rows = std::max(rows, depth + 1);
			const float left =
				origin.x + std::clamp((span->StartMilliseconds - first) * scale, 0.0f, graphWidth);
			const float right = std::clamp(
				left + std::max(span->Milliseconds * scale, 1.0f), left + 1.0f, origin.x + graphWidth
			);
			const float top = origin.y + static_cast<float>(depth) * rowHeight;
			const ImVec2 upper(left, top);
			const ImVec2 lower(right, top + rowHeight);
			draw->AddRectFilled(upper, lower, engine::ui::AccentColour());
			if (ImGui::IsMouseHoveringRect(upper, lower)) {
				hovered = span;
				draw->AddRect(upper, lower, engine::ui::BrightColour());
			}
			if (right - left > engine::ui::Scaled(42.0f)) {
				draw->PushClipRect(upper, lower, true);
				draw->AddText(
					ImVec2(left + engine::ui::Scaled(3.0f), top + engine::ui::Scaled(1.0f)),
					IM_COL32(0, 0, 0, 255),
					span->Name.data(),
					span->Name.data() + span->Name.size()
				);
				draw->PopClipRect();
			}
		}

		ImGui::Dummy(ImVec2(graphWidth, static_cast<float>(rows) * rowHeight));
		if (hovered != nullptr) {
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(hovered->Name.data(), hovered->Name.data() + hovered->Name.size());
			ImGui::Text("%.3f ms", static_cast<double>(hovered->Milliseconds));
			ImGui::EndTooltip();
		}

		struct BindingUse {
			std::string_view Name;
			uint32_t Calls = 0;
			float Milliseconds = 0.0f;
		};
		std::vector<BindingUse> bindings;
		for (const FrameSpan *span : spans) {
			if (ScriptingFilter[0] != '\0' &&
				span->Name.find(ScriptingFilter.data()) == std::string_view::npos) {
				continue;
			}

			auto found = std::find_if(bindings.begin(), bindings.end(), [span](const BindingUse &use) {
				return use.Name == span->Name;
			});
			if (found == bindings.end()) {
				bindings.push_back(BindingUse{span->Name, 1, span->Milliseconds});
			} else {
				found->Calls++;
				found->Milliseconds += span->Milliseconds;
			}
		}
		std::sort(bindings.begin(), bindings.end(), [](const BindingUse &left, const BindingUse &right) {
			return left.Milliseconds > right.Milliseconds;
		});

		ImGui::Separator();
		ImGui::TextDisabled("native binding activity this frame");
		if (bindings.empty()) {
			ImGui::TextDisabled("no native bindings matched");
		} else if (ImGui::BeginTable(
					   "##binding-uses", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
				   )) {
			ImGui::TableSetupColumn("binding", ImGuiTableColumnFlags_WidthStretch, 0.5f);
			ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_WidthStretch, 0.15f);
			ImGui::TableSetupColumn("active frame time", ImGuiTableColumnFlags_WidthStretch, 0.35f);
			ImGui::TableHeadersRow();

			for (const BindingUse &binding : bindings) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(binding.Name.data(), binding.Name.data() + binding.Name.size());
				ImGui::TableNextColumn();
				ImGui::Text("%u", binding.Calls);
				ImGui::TableNextColumn();

				const float share = std::clamp(binding.Milliseconds / widthMilliseconds, 0.0f, 1.0f);
				char label[48];
				std::snprintf(label, sizeof(label), "%.3f ms", static_cast<double>(binding.Milliseconds));
				ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), label);
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}
}
