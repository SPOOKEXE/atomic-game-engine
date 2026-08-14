// The Demo Nodes panel: a canvas over `Vendor::nodegraph`, and the pictures it
// draws.
//
// **A demo and not a feature, said out loud.** Nothing in the engine reads this
// graph. It is here ahead of the two node systems the roadmap wants — the render
// pipeline as an editor, and `Engine::bakegraph`'s pipeline documents — and what
// it proves is that the vendored library is enough for both: a registry, a model
// with a cycle guard, a layout, a canvas, and an evaluator that can run
// something slow without stopping the frame.
//
// **The node set is not here and neither is the canvas.** Both are
// `mono.vendor/nodegraph`, which is where this design lives after D00113 — the
// terrain types, the colouriser and the two async tasks are its
// `demo/Nodes.cpp`, and this file is only the panel around them. What is left in
// this repository is what an engine has and a library cannot: a texture for a
// picture, a theme, a file dialog and an undo stack.
//
// ## The panel
//
// A canvas, a breadcrumb bar when the view is inside a fold, and three tabs
// answering three different questions about one selection: **Library** — what
// can I add; **Inspector** — what is this and what did it make; **Types** —
// what will connect to what. They are tabs and not three panels because only
// one of the three is being asked at a time.
//
// The inspector's middle is the only part that varies by node type, and it is
// dispatched through `nodegraph::Inspectors` rather than switched on here.
// Around it the panel draws what every type has: a name, how long it took, its
// knobs and its ports — drawn from `WidgetsOf`/`InputsOf`/`OutputsOf` so a
// compressed node shows the interface it derived rather than the empty
// declaration it was placed from.
//
// **Undo is whole documents rather than a command log.** `studio/Commands.hpp`
// takes the other side of that trade for the scene, and for its reason: a scene
// is large and an edit is small. This graph serialises to a few hundred bytes,
// and a snapshot cannot be wrong about what it reverses.
//
// @tier client

#include <engine/assets/Texture.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <nodegraph/Demo.hpp>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Inspect.hpp>
#include <nodegraph/Layout.hpp>
#include <nodegraph/Preview.hpp>
#include <nodegraph/Serialize.hpp>
#include <sstream>
#include <string>
#include <studio/Editor.hpp>
#include <vector>

namespace studio {

	void ApplyNodeChrome() {
		nodegraph::Chrome &chrome = nodegraph::HostChrome();
		chrome.Muted = engine::ui::MutedColour();
		chrome.Accent = engine::ui::AccentColour();
		chrome.Warning = engine::ui::WarningColour();
		chrome.Scale = engine::ui::Scaled(1.0f);
	}

	bool NodePreviewTexture(const nodegraph::PreviewImage &image, engine::assets::TextureData &out) {
		if (!image.Valid()) {
			return false;
		}

		out.Width = image.Side;
		out.Height = image.Side;
		out.Format = engine::assets::TextureFormat::RGBA8;
		out.Pixels.resize(image.Rgba.size());
		std::memcpy(out.Pixels.data(), image.Rgba.data(), image.Rgba.size());
		return true;
	}

	void Editor::PumpNodeDemoImages() {
		// **A ceiling, and it drops the lot rather than the oldest.** Every
		// preview is a texture in video memory, and a session spent dragging one
		// slider produces a new result — and so a new key — on every frame it
		// moves. Thumbnails evict least-recently-drawn because a store is browsed
		// in one direction; this is a graph whose visible set is small and
		// rebuilt in a frame, so the simple rule costs one frame of pictures and
		// needs no bookkeeping to get wrong.
		//
		// **Between frames, where `PumpThumbnails` runs**, because a texture
		// released while a draw list still names it is a use-after-free on the
		// GPU rather than a missing picture.
		constexpr size_t PREVIEW_CEILING = 64;

		// The orbit view's displaced texture, now that no draw list names it.
		if (NodeDemoOrbitStale.IsValid()) {
			Renderer.DropTexture(NodeDemoOrbitStale);
			NodeDemoOrbitStale = engine::core::Name{};
		}

		if (NodeDemoTextures.size() <= PREVIEW_CEILING) {
			return;
		}
		DropNodeDemoImages();
	}

	void *Editor::NodeDemoImage(uint64_t key, const std::function<bool(nodegraph::PreviewImage &)> &make) {
		// **Held under the hash the payload was computed at**, so panning and
		// zooming cost a lookup, an edit makes a new key rather than overwriting
		// the old one, and two nodes computing the same thing share one texture.
		if (const auto found = NodeDemoTextures.find(key); found != NodeDemoTextures.end()) {
			return found->second;
		}

		nodegraph::PreviewImage image;
		engine::assets::TextureData texture;
		if (!make(image) || !NodePreviewTexture(image, texture)) {
			// **Remembered as null rather than retried.** A payload with no
			// picture would otherwise be converted on every frame it is drawn.
			NodeDemoTextures.emplace(key, nullptr);
			return nullptr;
		}

		// **Prefixed, so a preview can never be sampled as content** — the same
		// rule `ThumbnailTextureName` states: the renderer resolves a part's
		// texture out of this table by name, and an unprefixed key would let a
		// node's thumbnail become a wall.
		const engine::core::Name name("studio.nodepreview/" + std::to_string(key));
		if (!Renderer.AddTexture(name, texture)) {
			NodeDemoTextures.emplace(key, nullptr);
			return nullptr;
		}

		void *handle = Renderer.TextureHandle(name);
		NodeDemoTextures.emplace(key, handle);
		NodeDemoTextureNames.emplace(key, name);
		return handle;
	}

	void *
	Editor::NodeDemoOrbitImage(uint64_t key, const std::function<bool(nodegraph::PreviewImage &)> &make) {
		// Already showing it. An orbit that came to rest costs a comparison a
		// frame and nothing else.
		if (key == NodeDemoOrbitKey && NodeDemoOrbitName.IsValid()) {
			return NodeDemoOrbitHandle;
		}

		nodegraph::PreviewImage image;
		engine::assets::TextureData texture;
		if (!make(image) || !NodePreviewTexture(image, texture)) {
			NodeDemoOrbitKey = key;
			NodeDemoOrbitHandle = nullptr;
			return nullptr;
		}

		// **A fresh name, and the old one is only marked.** Replacing under one
		// name would release a texture this frame's draw list already points at.
		const engine::core::Name name("studio.nodeorbit/" + std::to_string(NodeDemoOrbitSerial++));
		if (!Renderer.AddTexture(name, texture)) {
			NodeDemoOrbitKey = key;
			NodeDemoOrbitHandle = nullptr;
			return nullptr;
		}

		if (NodeDemoOrbitName.IsValid()) {
			// Only ever one waiting: the previous frame's has already been
			// released by the pump between then and now.
			if (NodeDemoOrbitStale.IsValid()) {
				Renderer.DropTexture(NodeDemoOrbitStale);
			}
			NodeDemoOrbitStale = NodeDemoOrbitName;
		}

		NodeDemoOrbitName = name;
		NodeDemoOrbitHandle = Renderer.TextureHandle(name);
		NodeDemoOrbitKey = key;
		return NodeDemoOrbitHandle;
	}

	void Editor::DropNodeDemoImages() {
		for (const auto &[key, name] : NodeDemoTextureNames) {
			Renderer.DropTexture(name);
		}
		NodeDemoTextureNames.clear();
		NodeDemoTextures.clear();

		if (NodeDemoOrbitStale.IsValid()) {
			Renderer.DropTexture(NodeDemoOrbitStale);
			NodeDemoOrbitStale = engine::core::Name{};
		}
		if (NodeDemoOrbitName.IsValid()) {
			Renderer.DropTexture(NodeDemoOrbitName);
			NodeDemoOrbitName = engine::core::Name{};
		}
		NodeDemoOrbitHandle = nullptr;
		NodeDemoOrbitKey = 0;
	}
	// --- the history ----------------------------------------------------------

	void Editor::CommitNodeDemo() {
		// **Compared against what the graph already read as.** A commit is
		// called from every gesture that might have changed something, and most
		// of them did not — a drag that moved a node one pixel and back is not
		// an undo step.
		std::string now = nodegraph::Save(NodeDemoGraph);
		if (now == NodeDemoLast) {
			return;
		}

		if (!NodeDemoLast.empty()) {
			NodeDemoPast.push_back(std::move(NodeDemoLast));
		}
		NodeDemoLast = std::move(now);
		NodeDemoFuture.clear();

		// A ceiling, because every entry is a whole document and somebody
		// dragging sliders for an hour should not be holding all of it.
		constexpr size_t DEPTH = 100;
		if (NodeDemoPast.size() > DEPTH) {
			NodeDemoPast.erase(NodeDemoPast.begin());
		}
	}

	void Editor::RestoreNodeDemo(const std::string &document) {
		std::string error;
		if (!nodegraph::Load(document, NodeDemoGraph, error)) {
			NodeDemoSaid = error;
			return;
		}

		// **The selection is dropped rather than repointed.** `Load` hands out
		// new ids, so a held one names a different node — which is worse than
		// nothing selected, because it looks like it worked.
		NodeDemoCanvas.Select(nodegraph::NO_NODE);
		NodeDemoSignature = 0;
	}

	void Editor::UndoNodeDemo() {
		if (NodeDemoPast.empty()) {
			NodeDemoSaid = "nothing to undo";
			return;
		}
		NodeDemoFuture.push_back(NodeDemoLast);
		NodeDemoLast = NodeDemoPast.back();
		NodeDemoPast.pop_back();
		RestoreNodeDemo(NodeDemoLast);
	}

	void Editor::RedoNodeDemo() {
		if (NodeDemoFuture.empty()) {
			NodeDemoSaid = "nothing to redo";
			return;
		}
		NodeDemoPast.push_back(NodeDemoLast);
		NodeDemoLast = NodeDemoFuture.back();
		NodeDemoFuture.pop_back();
		RestoreNodeDemo(NodeDemoLast);
	}

	std::string Editor::ExportNodeDemoImage(nodegraph::NodeId id) {
		const nodegraph::Node *node = NodeDemoGraph.Find(id);
		const nodegraph::NodeType *type = node == nullptr ? nullptr : nodegraph::NodeTypes::Find(node->Type);
		if (type == nullptr) {
			return "nothing selected";
		}

		// The first output that makes a picture. A node with several is asking
		// for a port picker, which is a dialog for a thing nobody does twice.
		nodegraph::PreviewImage image;
		for (const nodegraph::PortSpec &port : type->Outputs) {
			const std::any *payload = NodeDemoRunner.Output(id, port.Name);
			if (payload != nullptr && nodegraph::PictureOf(type, port.Type, *payload, image) &&
				image.Valid()) {
				break;
			}
			image = nodegraph::PreviewImage{};
		}
		if (!image.Valid()) {
			return "that node has produced no picture";
		}

		const std::string path = std::string(NodeDemoPath) + ".png";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) {
			return "could not write " + path;
		}

		const std::vector<uint8_t> encoded = nodegraph::EncodePng(image);
		out.write(
			reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size())
		);
		return "exported " + path;
	}

	// --- the panel ------------------------------------------------------------

	void Editor::DrawNodeDemo() {
		if (!ShowNodeDemo) {
			return;
		}

		ApplyNodeChrome();

		if (!ImGui::Begin("Demo Nodes", &ShowNodeDemo, ImGuiWindowFlags_MenuBar)) {
			ImGui::End();
			return;
		}

		// **Built on the first open rather than at start-up.** A panel nobody
		// opens must cost nothing, which is this program's rule for every panel
		// that answers a question occasionally — and a graph built before the
		// node types were registered would be an empty one.
		if (NodeDemoGraph.Nodes().empty() && NodeDemoLast.empty()) {
			nodegraph::BuildDemoGraph(NodeDemoGraph);
			NodeDemoCanvas.Observe(&NodeDemoRunner);
			NodeDemoCanvas.Images([this](
									  uint64_t key, const std::function<bool(nodegraph::PreviewImage &)> &make
								  ) { return NodeDemoImage(key, make); });

			// What the canvas cannot decide for itself: when an edit becomes an
			// undo step, and what "run this one again" means.
			NodeDemoCanvas.Signals.Changed = [this] { CommitNodeDemo(); };
			NodeDemoCanvas.Signals.Rerun = [this](nodegraph::NodeId) {
				// **The whole cache and not one entry.** A result is keyed by a
				// hash of the node *and everything above it*, so "recompute this
				// one" would have to invalidate every hash that folded it in —
				// and the graph is small enough that starting over is cheaper
				// than the bookkeeping to do it precisely.
				NodeDemoRunner.Forget();
				DropNodeDemoImages();
				NodeDemoSignature = 0;
			};

			NodeDemoSignature = 0;
			NodeDemoLast = nodegraph::Save(NodeDemoGraph);
		}

		DrawNodeDemoBar();
		DrawNodeDemoCrumbs();

		// The canvas on the left, the three tabs down the right. The canvas says
		// how the graph is wired; the panel says what came out of it, what can be
		// added to it, and what will connect to what.
		const float inspector = engine::ui::Scaled(250.0f);
		const float bar = ImGui::GetTextLineHeightWithSpacing();
		const ImVec2 room = ImGui::GetContentRegionAvail();

		if (ImGui::BeginChild("##nodes", ImVec2(room.x - inspector, room.y - bar), ImGuiChildFlags_None)) {
			NodeDemoCanvas.Draw(NodeDemoGraph);
		}
		ImGui::EndChild();

		ImGui::SameLine();

		if (ImGui::BeginChild("##panel", ImVec2(0.0f, room.y - bar), ImGuiChildFlags_Borders)) {
			if (ImGui::BeginTabBar("##tabs")) {
				if (ImGui::BeginTabItem("Library")) {
					NodeDemoTab = 0;
					DrawNodeDemoLibrary();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Inspector")) {
					NodeDemoTab = 1;
					DrawNodeDemoInspector();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Types")) {
					NodeDemoTab = 2;
					DrawNodeDemoTypes();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}
		ImGui::EndChild();

		DrawNodeDemoStatus();

		// **Re-run when the graph moved, and every frame while anything is
		// working.** The signature covers parameters and topology and
		// deliberately not position, so dragging a node recomputes nothing —
		// while a running node needs a call a frame to collect its result and to
		// move its bar.
		const uint64_t now = NodeDemoGraph.Signature();
		const bool changed = now != NodeDemoSignature;

		if (NodeDemoRunner.Busy() || (changed && NodeDemoLive) || (changed && NodeDemoSignature == 0)) {
			NodeDemoSignature = now;

			const auto began = std::chrono::steady_clock::now();
			NodeDemoReport = NodeDemoRunner.Run(NodeDemoGraph);
			NodeDemoRunMs =
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();

			// **Accumulated rather than taken from the last run.** A hit rate
			// over one frame of a settled graph is always 100%, which is a
			// number that tells nobody anything.
			NodeDemoHits += NodeDemoReport.Cached;
			NodeDemoMisses += NodeDemoReport.Evaluated + NodeDemoReport.Started;
		}

		ImGui::End();
	}

	void Editor::DrawNodeDemoBar() {
		if (!ImGui::BeginMenuBar()) {
			return;
		}

		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New graph")) {
				NodeDemoGraph.Clear();
				NodeDemoRunner.Forget();
				DropNodeDemoImages();
				NodeDemoCanvas.Select(nodegraph::NO_NODE);
				NodeDemoSignature = 0;
				CommitNodeDemo();
			}
			if (ImGui::MenuItem("Reset to the demo graph")) {
				nodegraph::BuildDemoGraph(NodeDemoGraph);
				NodeDemoRunner.Forget();
				DropNodeDemoImages();
				NodeDemoCanvas.Select(nodegraph::NO_NODE);
				NodeDemoSignature = 0;
				CommitNodeDemo();
			}

			ImGui::Separator();
			ImGui::SetNextItemWidth(engine::ui::Scaled(220.0f));
			ImGui::InputText("##path", NodeDemoPath, sizeof(NodeDemoPath));

			if (ImGui::MenuItem("Save")) {
				// **Written as the text `nodegraph::Save` produces**, which is three
				// flat lists and no parser — a graph anybody can read in a diff
				// and hand-edit without a schema.
				std::ofstream out(NodeDemoPath, std::ios::binary | std::ios::trunc);
				if (out) {
					out << nodegraph::Save(NodeDemoGraph);
					NodeDemoSaid = std::string("saved ") + NodeDemoPath;
				} else {
					NodeDemoSaid = std::string("could not write ") + NodeDemoPath;
				}
			}
			if (ImGui::MenuItem("Load")) {
				std::ifstream in(NodeDemoPath, std::ios::binary);
				if (in) {
					std::ostringstream held;
					held << in.rdbuf();

					std::string error;
					if (nodegraph::Load(held.str(), NodeDemoGraph, error)) {
						NodeDemoRunner.Forget();
						DropNodeDemoImages();
						NodeDemoCanvas.Select(nodegraph::NO_NODE);
						NodeDemoSignature = 0;
						NodeDemoPast.clear();
						NodeDemoFuture.clear();
						NodeDemoLast = nodegraph::Save(NodeDemoGraph);
						NodeDemoSaid = std::string("loaded ") + NodeDemoPath;
					} else {
						NodeDemoSaid = error;
					}
				} else {
					NodeDemoSaid = std::string("no such file: ") + NodeDemoPath;
				}
			}

			ImGui::Separator();

			// A real PNG, written by `EncodePng` — stored deflate blocks, so
			// nothing new is linked for it.
			const std::vector<nodegraph::NodeId> &chosen = NodeDemoCanvas.Selection();
			if (ImGui::MenuItem("Export the selected node's picture", nullptr, false, chosen.size() == 1)) {
				NodeDemoSaid = ExportNodeDemoImage(chosen.front());
			}
			ImGui::EndMenu();
		}

		ImGui::BeginDisabled(NodeDemoPast.empty());
		if (ImGui::SmallButton("undo")) {
			UndoNodeDemo();
		}
		ImGui::EndDisabled();
		ImGui::BeginDisabled(NodeDemoFuture.empty());
		ImGui::SameLine();
		if (ImGui::SmallButton("redo")) {
			RedoNodeDemo();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled("|");

		ImGui::SameLine();
		ImGui::BeginDisabled(NodeDemoCanvas.Selection().size() < 2);
		if (ImGui::SmallButton("group")) {
			NodeDemoCanvas.GroupSelection(NodeDemoGraph);
			CommitNodeDemo();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::SmallButton("ungroup")) {
			NodeDemoCanvas.UngroupSelection(NodeDemoGraph);
			CommitNodeDemo();
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(NodeDemoCanvas.Selection().size() < 2);
		if (ImGui::SmallButton("compress")) {
			NodeDemoCanvas.CompressSelection(NodeDemoGraph);
			CommitNodeDemo();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Fold the selection into one node whose ports are derived from how it was wired "
				"(Ctrl+Shift+C)"
			);
		}

		ImGui::SameLine();
		if (ImGui::SmallButton("expand")) {
			NodeDemoCanvas.ExpandSelection(NodeDemoGraph);
			CommitNodeDemo();
		}

		ImGui::SameLine();
		ImGui::TextDisabled("|");

		ImGui::SameLine();
		if (ImGui::SmallButton("collapse")) {
			NodeDemoCanvas.Collapse(NodeDemoGraph, true);
			CommitNodeDemo();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("uncollapse")) {
			NodeDemoCanvas.Collapse(NodeDemoGraph, false);
			CommitNodeDemo();
		}

		ImGui::SameLine();
		ImGui::Checkbox("snap", &NodeDemoCanvas.Snap);

		ImGui::SameLine();
		ImGui::TextDisabled("|");

		// **Live is the default and Build is the escape.** A graph of cheap
		// filters wants to follow the slider; one with a six-second task in it
		// does not want to start over on every twitch, and this is the switch
		// between those.
		ImGui::SameLine();
		ImGui::Checkbox("live", &NodeDemoLive);
		ImGui::SameLine();
		ImGui::BeginDisabled(NodeDemoLive);
		if (ImGui::SmallButton("build")) {
			NodeDemoSignature = 0;
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::SmallButton("fit")) {
			NodeDemoCanvas.Fit(NodeDemoGraph);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("forget cache")) {
			NodeDemoRunner.Forget();
			DropNodeDemoImages();
			NodeDemoHits = 0;
			NodeDemoMisses = 0;
			NodeDemoSignature = 0;
		}

		// Whichever the last thing to say something was. The canvas refuses
		// links and the file menu reports; both are one line and neither is
		// worth a dialog.
		const std::string &said =
			!NodeDemoCanvas.LastRefusal.empty() ? NodeDemoCanvas.LastRefusal : NodeDemoSaid;
		if (!said.empty()) {
			ImGui::SameLine();
			ImGui::PushStyleColor(
				ImGuiCol_Text,
				NodeDemoCanvas.LastRefusal.empty() ? engine::ui::MutedColour() : engine::ui::WarningColour()
			);
			ImGui::TextUnformatted(said.c_str());
			ImGui::PopStyleColor();
		}

		ImGui::EndMenuBar();
	}

	void Editor::DrawNodeDemoCrumbs() {
		const std::vector<nodegraph::NodeId> &path = NodeDemoCanvas.Path();
		if (path.empty()) {
			// **Nothing at the root, rather than a bar reading "Root".** A row
			// that is always there and only ever says one thing is a row of
			// height nobody gets back.
			return;
		}

		if (ImGui::SmallButton("Root")) {
			NodeDemoCanvas.Ascend(NodeDemoGraph, 0);
		}

		for (size_t depth = 0; depth < path.size(); depth++) {
			const nodegraph::Node *node = NodeDemoGraph.Find(path[depth]);
			ImGui::SameLine();
			ImGui::TextDisabled("/");
			ImGui::SameLine();

			const std::string label =
				node == nullptr || node->Label.empty() ? std::string("Subgraph") : node->Label;
			ImGui::PushID(static_cast<int>(depth));

			// The last one is where we are, so it is not a way to go anywhere.
			if (depth + 1 == path.size()) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
				ImGui::TextUnformatted(label.c_str());
				ImGui::PopStyleColor();
			} else if (ImGui::SmallButton(label.c_str())) {
				NodeDemoCanvas.Ascend(NodeDemoGraph, depth + 1);
			}
			ImGui::PopID();
		}

		ImGui::Separator();
	}

	void Editor::DrawNodeDemoStatus() {
		const auto say = [](const char *label, const std::string &value) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(label);
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 0.5f);
			ImGui::TextUnformatted(value.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("·");
			ImGui::SameLine();
		};

		char text[64];

		std::snprintf(text, sizeof(text), "%.0f%%", static_cast<double>(NodeDemoCanvas.Zoom() * 100.0f));
		say("zoom", text);

		std::snprintf(text, sizeof(text), "%zu", NodeDemoGraph.Nodes().size());
		say("nodes", text);

		std::snprintf(text, sizeof(text), "%zu", NodeDemoGraph.Links().size());
		say("links", text);

		if (!NodeDemoCanvas.Selection().empty()) {
			std::snprintf(text, sizeof(text), "%zu", NodeDemoCanvas.Selection().size());
			say("selected", text);
		}

		// **The hit rate, because it is the one number that says the cache
		// works.** A graph whose hashes never settle recomputes for ever, and
		// that shows up here as a rate falling towards zero long before anybody
		// notices the frame rate.
		const size_t asked = NodeDemoHits + NodeDemoMisses;
		std::snprintf(text, sizeof(text), "%zu/%zu", NodeDemoHits, asked);
		say("cache", text);

		std::snprintf(text, sizeof(text), "%.1f ms", NodeDemoRunMs);
		say("run", text);

		if (NodeDemoReport.Running > 0 || NodeDemoReport.Waiting > 0) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
			ImGui::Text("%zu running  %zu waiting", NodeDemoReport.Running, NodeDemoReport.Waiting);
			ImGui::PopStyleColor();
		} else {
			ImGui::PushStyleColor(
				ImGuiCol_Text, NodeDemoLive ? engine::ui::AccentColour() : engine::ui::MutedColour()
			);
			ImGui::TextUnformatted(NodeDemoLive ? "live" : "manual");
			ImGui::PopStyleColor();
		}
	}

	// --- the tabs -------------------------------------------------------------

	void Editor::DrawNodeDemoLibrary() {
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##filter", "filter nodes", NodeDemoFilter, sizeof(NodeDemoFilter));

		const std::string wanted = NodeDemoFilter;
		const auto matchesName = [&wanted](const std::string &name) {
			if (wanted.empty()) {
				return true;
			}
			std::string haystack = name;
			std::string needle = wanted;
			for (char &letter : haystack) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			for (char &letter : needle) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			return haystack.find(needle) != std::string::npos;
		};
		const auto matches = [&wanted](const nodegraph::NodeType &type) {
			if (wanted.empty()) {
				return true;
			}
			std::string haystack = type.Title + " " + type.Category + " " + type.Id;
			std::string needle = wanted;
			for (char &letter : haystack) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			for (char &letter : needle) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}
			return haystack.find(needle) != std::string::npos;
		};

		// **The custom types first**, because they are the ones this graph is
		// about — a library of eight built-in categories with one entry
		// somebody made is a list whose useful row is at the bottom.
		if (!NodeDemoGraph.Templates().empty()) {
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			if (ImGui::CollapsingHeader("Custom")) {
				std::string dropped;
				for (const nodegraph::Graph::Template &held : NodeDemoGraph.Templates()) {
					if (!matchesName(held.Name)) {
						continue;
					}
					ImGui::PushID(held.Name.c_str());
					if (ImGui::SmallButton("x")) {
						dropped = held.Name;
					}
					ImGui::SameLine();
					if (ImGui::Selectable(held.Name.c_str())) {
						NodeDemoCanvas.Place(NodeDemoGraph, held.Document);
						CommitNodeDemo();
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip(
							"place a copy — each one is independent of the fold it was made from"
						);
					}
					ImGui::PopID();
				}
				if (!dropped.empty()) {
					NodeDemoGraph.Forget(dropped);
					CommitNodeDemo();
				}
			}
		}

		for (const std::string &category : nodegraph::NodeTypes::Categories()) {
			size_t survived = 0;
			for (const nodegraph::NodeType &type : nodegraph::NodeTypes::All()) {
				survived += !type.Hidden && type.Category == category && matches(type) ? 1 : 0;
			}
			if (survived == 0) {
				continue;
			}

			// Open by default, and a filter forces it open — a search that left
			// its answers behind a closed heading would be a search that failed.
			ImGui::SetNextItemOpen(true, wanted.empty() ? ImGuiCond_Once : ImGuiCond_Always);
			if (!ImGui::CollapsingHeader(category.c_str())) {
				continue;
			}

			for (const nodegraph::NodeType &type : nodegraph::NodeTypes::All()) {
				if (type.Hidden || type.Category != category || !matches(type)) {
					continue;
				}

				// The accent bar, so the row reads as the node it makes.
				const ImVec2 at = ImGui::GetCursorScreenPos();
				const float mark = ImGui::GetTextLineHeight();
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(at.x, at.y + mark * 0.15f),
					ImVec2(at.x + mark * 0.28f, at.y + mark * 0.85f),
					ImGui::ColorConvertFloat4ToU32(ImVec4(type.Accent.R, type.Accent.G, type.Accent.B, 1.0f)),
					1.0f
				);
				ImGui::Dummy(ImVec2(mark * 0.28f, mark));
				ImGui::SameLine();

				ImGui::PushID(type.Id.c_str());
				if (ImGui::Selectable(type.Title.c_str())) {
					// **Placed in the middle of the view rather than at the
					// origin**, so adding a node while panned somewhere else does
					// not put it off screen with no clue where it went.
					const nodegraph::NodeId made = NodeDemoGraph.Add(type.Id, 0.0f, 0.0f);
					if (made != nodegraph::NO_NODE) {
						NodeDemoGraph.Find(made)->Owner = NodeDemoCanvas.Inside();
						NodeDemoCanvas.Select(made);
						NodeDemoCanvas.Centre(NodeDemoGraph, made);
						CommitNodeDemo();
					}
				}
				if (ImGui::IsItemHovered() && !type.Subtitle.empty()) {
					ImGui::SetTooltip("%s", type.Subtitle.c_str());
				}
				ImGui::PopID();
			}
		}
	}

	void Editor::DrawNodeDemoInspector() {
		if (const nodegraph::GroupId frame = NodeDemoCanvas.SelectedGroup(); frame != nodegraph::NO_GROUP) {
			const nodegraph::Group *group = NodeDemoGraph.FindGroup(frame);
			if (group != nullptr) {
				ImGui::TextUnformatted(group->Title.c_str());
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::Text("a frame around %zu nodes", group->Members.size());
				ImGui::PopStyleColor();

				ImGui::Separator();
				if (ImGui::Button("ungroup")) {
					NodeDemoCanvas.UngroupSelection(NodeDemoGraph);
					CommitNodeDemo();
				}
				return;
			}
		}

		const std::vector<nodegraph::NodeId> &chosen = NodeDemoCanvas.Selection();
		if (chosen.empty()) {
			ImGui::TextDisabled("select a node");
			ImGui::Spacing();
			ImGui::TextWrapped(
				"Drag a port to a port to connect. Drop a wire in empty space for a palette of what "
				"could go there. Right click for a menu, Tab for the palette, double click a node to "
				"collapse it. Del removes, F fits, Ctrl+D duplicates, Ctrl+G frames a selection."
			);
			return;
		}

		if (chosen.size() > 1) {
			ImGui::Text("%zu nodes selected", chosen.size());
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped("Ctrl+G frames them, Ctrl+D duplicates them, Del removes them.");
			ImGui::PopStyleColor();
			return;
		}

		nodegraph::Node *node = NodeDemoGraph.Find(chosen.front());
		if (node == nullptr) {
			return;
		}
		const nodegraph::NodeType *type = nodegraph::NodeTypes::Find(node->Type);
		if (type == nullptr) {
			ImGui::TextDisabled("%s", node->Type.c_str());
			ImGui::TextWrapped("no node type of that name is registered");
			return;
		}

		// --- what it is -------------------------------------------------------

		char title[128];
		std::snprintf(
			title, sizeof(title), "%s", node->Label.empty() ? type->Title.c_str() : node->Label.c_str()
		);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##title", title, sizeof(title))) {
			// **Empty means "use the type's name" rather than an empty header.**
			// Clearing the box is how somebody undoes a rename.
			node->Label = title == type->Title ? std::string() : title;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			CommitNodeDemo();
		}

		const nodegraph::NodeStatus status = NodeDemoRunner.Status(node->Id);

		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		if (status.Cached) {
			ImGui::Text("%s  ·  cache hit", node->Type.c_str());
		} else if (status.State == nodegraph::NodeState::Done) {
			ImGui::Text("%s  ·  %.1f ms", node->Type.c_str(), status.Milliseconds);
		} else {
			ImGui::TextUnformatted(node->Type.c_str());
		}
		ImGui::PopStyleColor();

		if (!type->Subtitle.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped("%s", type->Subtitle.c_str());
			ImGui::PopStyleColor();
		}
		if (status.State == nodegraph::NodeState::Failed) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::TextUnformatted("it raised");
			ImGui::PopStyleColor();
		}

		// --- what it made -----------------------------------------------------
		//
		// **Dispatched rather than switched on.** A field node gets its picture
		// with its inputs beside it, an async one gets its stages, and a readout
		// gets its number — and a node type added tomorrow gets whichever of
		// those fits what it produced, with nothing here changing.
		nodegraph::Inspection what;
		what.Node = node;
		what.Type = type;
		what.Graph = &NodeDemoGraph;
		what.Runner = &NodeDemoRunner;
		what.Images = [this](uint64_t key, const std::function<bool(nodegraph::PreviewImage &)> &make) {
			return NodeDemoImage(key, make);
		};
		what.Orbit = [this](uint64_t key, const std::function<bool(nodegraph::PreviewImage &)> &make) {
			return NodeDemoOrbitImage(key, make);
		};

		if (const nodegraph::InspectorFn *draw = nodegraph::Inspectors::For(what); draw != nullptr) {
			(*draw)(what);
		}

		// --- what it holds ----------------------------------------------------

		if (node->Compressed()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::SeparatorText("folded");
			ImGui::PopStyleColor();

			ImGui::Text("%zu nodes inside", NodeDemoGraph.Contents(node->Id).size());

			if (ImGui::Button("enter")) {
				NodeDemoCanvas.Enter(NodeDemoGraph, node->Id);
			}
			ImGui::SameLine();
			if (ImGui::Button("expand in place")) {
				NodeDemoCanvas.ExpandSelection(NodeDemoGraph);
				CommitNodeDemo();
				return;
			}

			// **Filed under a name, into the document.** A library type carried
			// by the process would make a saved graph unopenable anywhere else;
			// carried by the graph, one file is the whole thing.
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint(
				"##typename", "name it to save as a type", NodeDemoTypeName, sizeof(NodeDemoTypeName)
			);
			ImGui::BeginDisabled(NodeDemoTypeName[0] == '\0');
			if (ImGui::Button("save as type")) {
				NodeDemoGraph.Remember(NodeDemoTypeName, nodegraph::SaveSubtree(NodeDemoGraph, node->Id));
				NodeDemoSaid = std::string("filed \"") + NodeDemoTypeName + "\" under Custom";
				NodeDemoTypeName[0] = '\0';
				CommitNodeDemo();
			}
			ImGui::EndDisabled();

			// **Which promoted knobs are shown.** A thirty-widget selection
			// folds into a node that should present the three that matter; this
			// is how the other twenty-seven are put away without losing them,
			// and it is why `Promotion::Exposed` is a flag rather than a delete.
			if (ImGui::TreeNode("exposed parameters")) {
				bool changed = false;
				for (nodegraph::Promotion &promotion : node->Promoted) {
					ImGui::PushID(promotion.Key.c_str());
					if (ImGui::Checkbox(promotion.Label.c_str(), &promotion.Exposed)) {
						changed = true;
					}
					ImGui::PopID();
				}
				ImGui::TreePop();
				if (changed) {
					CommitNodeDemo();
				}
			}
		}

		// --- its knobs --------------------------------------------------------

		if (!nodegraph::WidgetsOf(*node).empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::SeparatorText(node->Compressed() ? "promoted parameters" : "parameters");
			ImGui::PopStyleColor();

			if (DrawNodeDemoWidgets(*node)) {
				CommitNodeDemo();
			}
		}

		// --- its ports --------------------------------------------------------

		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::SeparatorText("ports");
		ImGui::PopStyleColor();

		if (nodegraph::InputsOf(*node).empty() && nodegraph::OutputsOf(*node).empty()) {
			ImGui::TextDisabled("none — this node is a note");
		}

		for (int side = 0; side < 2; side++) {
			const std::vector<nodegraph::PortSpec> ports =
				side == 0 ? nodegraph::InputsOf(*node) : nodegraph::OutputsOf(*node);
			for (const nodegraph::PortSpec &port : ports) {
				const nodegraph::DataType *carried = nodegraph::DataTypes::Find(port.Type);
				const ImVec4 tint = carried != nullptr
										? ImVec4(carried->Tint.R, carried->Tint.G, carried->Tint.B, 1.0f)
										: ImVec4(0.62f, 0.64f, 0.70f, 1.0f);

				const float mark = ImGui::GetTextLineHeight();
				const ImVec2 at = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddCircleFilled(
					ImVec2(at.x + mark * 0.35f, at.y + mark * 0.5f),
					mark * 0.24f,
					ImGui::ColorConvertFloat4ToU32(tint)
				);
				ImGui::Dummy(ImVec2(mark * 0.8f, mark));
				ImGui::SameLine();

				ImGui::Text("%s %s", side == 0 ? "in " : "out", port.Name.c_str());
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextUnformatted(carried != nullptr ? carried->Label.c_str() : port.Type.c_str());
				ImGui::PopStyleColor();

				// Whether anything is on it, which is the question a port table
				// is usually being read to answer.
				if (side == 0) {
					nodegraph::NodeId held = nodegraph::NO_NODE;
					std::string heldPort;
					const bool real =
						nodegraph::Actual(NodeDemoGraph, node->Id, port.Name, true, held, heldPort);
					if (!real || NodeDemoGraph.LinkInto(held, heldPort) == nullptr) {
						ImGui::SameLine();
						ImGui::TextDisabled("unconnected");
					}
				}
			}
		}
	}

	bool Editor::DrawNodeDemoWidgets(nodegraph::Node &node) {
		bool touched = false;

		// **The node's interface and not its type's.** A compressed node's knobs
		// are the ones it promoted out of its contents, and every read and write
		// here goes through `ValueOf`/`SetValue` so it lands on the node inside
		// rather than on the fold's own empty map.
		for (const nodegraph::WidgetSpec &spec : nodegraph::WidgetsOf(node)) {
			nodegraph::Value value = nodegraph::ValueOf(NodeDemoGraph, node.Id, spec);
			if (value.Kind != spec.Kind) {
				value = spec.Default;
			}
			const nodegraph::Value before = value;

			ImGui::PushID(spec.Key.c_str());
			ImGui::SetNextItemWidth(-1.0f);

			switch (spec.Kind) {
			case nodegraph::WidgetKind::Slider: {
				auto shown = static_cast<float>(value.Number);
				if (ImGui::SliderFloat(
						spec.Label.c_str(),
						&shown,
						static_cast<float>(spec.Minimum),
						static_cast<float>(spec.Maximum),
						"%.3f"
					)) {
					value.Number = static_cast<double>(shown);
					touched = true;
				}
				break;
			}
			case nodegraph::WidgetKind::Number: {
				auto shown = static_cast<float>(value.Number);
				if (ImGui::DragFloat(spec.Label.c_str(), &shown, static_cast<float>(spec.Step))) {
					value.Number = static_cast<double>(shown);
					touched = true;
				}
				break;
			}
			case nodegraph::WidgetKind::Toggle: {
				bool shown = value.Flag;
				if (ImGui::Checkbox(spec.Label.c_str(), &shown)) {
					value.Flag = shown;
					touched = true;
				}
				break;
			}
			case nodegraph::WidgetKind::Select: {
				// **A combo over the schema's options**, which is the same list
				// the canvas cycles through on a click. A second copy here is
				// how the two would come to disagree about what "volcanic" is.
				if (ImGui::BeginCombo(spec.Label.c_str(), value.Text.c_str())) {
					for (const std::string &option : spec.Options) {
						if (ImGui::Selectable(option.c_str(), option == value.Text)) {
							value.Text = option;
							touched = true;
						}
					}
					ImGui::EndCombo();
				}
				break;
			}
			case nodegraph::WidgetKind::Text: {
				char held[128];
				std::snprintf(held, sizeof(held), "%s", value.Text.c_str());
				if (ImGui::InputText(spec.Label.c_str(), held, sizeof(held))) {
					value.Text = held;
					touched = true;
				}
				break;
			}
			case nodegraph::WidgetKind::Colour: {
				float rgb[3] = {value.Tint.R, value.Tint.G, value.Tint.B};
				if (ImGui::ColorEdit3(spec.Label.c_str(), rgb)) {
					value.Tint.R = rgb[0];
					value.Tint.G = rgb[1];
					value.Tint.B = rgb[2];
					touched = true;
				}
				break;
			}
			}

			if (!(value == before)) {
				nodegraph::SetValue(NodeDemoGraph, node.Id, spec.Key, value);
			}
			ImGui::PopID();
		}

		// **Remembered until the widget is let go.** A slider drag is one undo
		// step, so it cannot be committed while the value is still moving — and
		// it cannot be committed on the frame the value last changed either,
		// because that frame is mid-drag. What is left is a flag that survives
		// to the frame nothing is being held.
		NodeDemoDirty = NodeDemoDirty || touched;
		if (NodeDemoDirty && !ImGui::IsAnyItemActive()) {
			NodeDemoDirty = false;
			return true;
		}
		return false;
	}

	void Editor::DrawNodeDemoTypes() {
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextWrapped(
			"A port carries a string identifier. A link is legal only where both ends share one, or "
			"one of them is the wildcard. Nothing here converts. Point at a row to pick it out on the "
			"canvas."
		);
		ImGui::PopStyleColor();

		ImGui::Spacing();

		// **Cleared every frame and set by whichever row is hovered**, so the
		// canvas never keeps a highlight for a panel nobody is looking at — and
		// so leaving the tab needs no notification.
		NodeDemoCanvas.Highlight.clear();

		for (const nodegraph::DataType &type : nodegraph::DataTypes::All()) {
			// Who uses it, which is what turns the table from a list of names
			// into something worth opening: a type nothing carries is a type
			// somebody registered and forgot.
			std::vector<nodegraph::NodeId> users;
			for (const nodegraph::Node &node : NodeDemoGraph.Nodes()) {
				const nodegraph::NodeType *declared = nodegraph::NodeTypes::Find(node.Type);
				if (declared == nullptr) {
					continue;
				}
				const auto carries = [&type](const std::vector<nodegraph::PortSpec> &ports) {
					for (const nodegraph::PortSpec &port : ports) {
						if (port.Type == type.Id) {
							return true;
						}
					}
					return false;
				};
				if (carries(declared->Inputs) || carries(declared->Outputs)) {
					users.push_back(node.Id);
				}
			}

			const float mark = ImGui::GetTextLineHeight();
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddCircleFilled(
				ImVec2(at.x + mark * 0.35f, at.y + mark * 0.7f),
				mark * 0.24f,
				ImGui::ColorConvertFloat4ToU32(ImVec4(type.Tint.R, type.Tint.G, type.Tint.B, 1.0f))
			);
			ImGui::Dummy(ImVec2(mark * 0.8f, 1.0f));
			ImGui::SameLine();

			char heading[128];
			std::snprintf(
				heading,
				sizeof(heading),
				"%s  ·  %zu node%s##%s",
				type.Label.c_str(),
				users.size(),
				users.size() == 1 ? "" : "s",
				type.Id.c_str()
			);

			const bool opened = ImGui::CollapsingHeader(heading);
			if (ImGui::IsItemHovered()) {
				NodeDemoCanvas.Highlight = type.Id;
			}
			if (!opened) {
				continue;
			}

			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped("%s", type.Id.c_str());
			ImGui::TextWrapped("%s", type.Description.c_str());
			if (!type.Preview) {
				ImGui::TextWrapped("no picture — this wire's payloads are read rather than looked at");
			}
			ImGui::PopStyleColor();

			for (const nodegraph::NodeId id : users) {
				const nodegraph::Node *node = NodeDemoGraph.Find(id);
				const nodegraph::NodeType *declared =
					node == nullptr ? nullptr : nodegraph::NodeTypes::Find(node->Type);
				if (declared == nullptr) {
					continue;
				}

				ImGui::PushID(static_cast<int>(id));
				if (ImGui::Selectable(node->Label.empty() ? declared->Title.c_str() : node->Label.c_str())) {
					// **Selects and centres**, which is what a row in a table of
					// who-uses-what is for: the question is where, and scrolling
					// a canvas by hand to find out is the thing this replaces.
					NodeDemoCanvas.Select(id);
					NodeDemoCanvas.Centre(NodeDemoGraph, id);
					NodeDemoTab = 1;
				}
				ImGui::PopID();
			}
		}
	}
	void Editor::DrawDemoTools() {
		// One demo today, and the row is the list of them. A tab whose contents
		// are a single button is what a second demo turns into a strip with no
		// other change.
		if (ImGui::Button("Demo Nodes", ImVec2(engine::ui::Scaled(110.0f), 0.0f))) {
			ShowNodeDemo = true;
			ImGui::SetWindowFocus("Demo Nodes");
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("A typed node graph with live evaluation — Vendor::nodegraph");
		}

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextUnformatted(
			"demos of engine parts, built to be looked at — nothing here changes the scene"
		);
		ImGui::PopStyleColor();
	}
}
