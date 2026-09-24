#include "ImageComposerInternal.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/bake/Pxcx.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Registry.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <studio/ImageGraph.hpp>
#include <unordered_set>
#include <utility>
#include <vector>

namespace studio {
	namespace {
		using engine::imagegraph::AuthoredValue;
		using engine::imagegraph::Colour;
		using engine::imagegraph::Diagnostic;
		using engine::imagegraph::Document;
		using engine::imagegraph::Image;
		using engine::imagegraph::Keyframe;
		using engine::imagegraph::Node;
		using engine::imagegraph::Output;
		using engine::imagegraph::Status;
		using engine::imagegraph::Value;

		constexpr uint32_t PREVIEW_MAXIMUM_DIMENSION = IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
		constexpr size_t PREVIEW_MAXIMUM_BYTES =
			static_cast<size_t>(PREVIEW_MAXIMUM_DIMENSION) * PREVIEW_MAXIMUM_DIMENSION * 4;
		constexpr size_t MAXIMUM_HISTORY = 128;

		struct State {
			bool Initialized = false;
			bool LivePreview = true;
			bool PreviewDirty = true;
			bool PreviewRequested = true;
			bool HaveGoodPreview = false;
			bool HaveScalarPreview = false;
			bool HaveActiveEdit = false;
			bool ShowAdvancedDiagnostics = false;
			uint64_t DocumentRevision = 1;
			uint64_t NextOutputId = 1;
			ImageGraphPlayback Playback;
			uint8_t TextureSlot = 1;
			engine::imagegraph::PortDirection GroupPortDirection = engine::imagegraph::PortDirection::Input;
			engine::imagegraph::ValueType GroupPortType = engine::imagegraph::ValueType::Image;
			uint64_t PreviewHash = 0;
			uint64_t PxcxThumbnailTextureHash = 0;
			uint32_t PreviewWidth = 0;
			uint32_t PreviewHeight = 0;
			ImGuiID ActiveEditId = 0;
			char Search[96] = {};
			char GraphName[256] = {};
			char PxcxPath[4096] = {};
			char AudioCapturePath[4096] = {};
			std::string SelectedOutput;
			std::string SinkMessage;
			std::string AdapterError;
			std::string PxcxPathDisplay;
			std::string PxcxOpenError;
			std::string GraphIoMessage;
			std::string PxcxThumbnailMessage;
			std::string AudioCapturePathDisplay;
			std::string AudioCaptureMessage;
			std::string ScalarPreviewPort;
			std::string SelectedGroup;
			std::string RouteFromEndpoint;
			std::string RouteFromPort;
			std::string RouteToEndpoint;
			std::string RouteToPort;
			std::array<char, 128> GroupName{};
			size_t ArrayPageOffset = 0;
			Document Authored;
			Document PxcxProjection;
			std::optional<engine::bake::PxcxArchive> ImportedPxcx;
			std::vector<Diagnostic> PxcxDiagnostics;
			Image Preview;
			double ScalarPreviewValue = 0.0;
			std::vector<engine::imagegraph::AudioCaptureFrame> AudioFrames;
			Image PxcxReferenceThumbnail;
			ImageGraphPreviewCache PreviewCache;
			Diagnostic LastDiagnostic;
			ImageGraphHistory History{MAXIMUM_HISTORY};
			ImageGraphCanvasIds Ids;
			nodegraph::Graph Graph;
			nodegraph::Canvas Canvas;
			Document EditBefore;
			engine::core::Name CurrentTexture;
			engine::core::Name RetiredTexture;
			engine::core::Name CurrentPxcxThumbnailTexture;
			engine::core::Name RetiredPxcxThumbnailTexture;
			uint8_t PxcxThumbnailTextureSlot = 1;
		};

		State &Composer() {
			static State state;
			return state;
		}

		void RequestPreview(State &state, bool immediate = false) {
			state.PreviewDirty = true;
			state.PreviewRequested = state.PreviewRequested || immediate;
		}

		void AuthoredDocumentChanged(State &state) {
			if (state.DocumentRevision == std::numeric_limits<uint64_t>::max()) {
				state.DocumentRevision = 1;
				state.PreviewCache.Clear();
			} else {
				state.DocumentRevision++;
			}
			RequestPreview(state);
		}

		Node *FindNode(Document &document, std::string_view id) {
			const auto found =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == id;
				});
			return found == document.Nodes.end() ? nullptr : &*found;
		}

		const Node *FindNode(const Document &document, std::string_view id) {
			const auto found =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == id;
				});
			return found == document.Nodes.end() ? nullptr : &*found;
		}

		AuthoredValue *FindValue(Node &node, std::string_view port) {
			const auto found =
				std::find_if(node.Values.begin(), node.Values.end(), [&](const AuthoredValue &value) {
					return value.Port == port;
				});
			return found == node.Values.end() ? nullptr : &*found;
		}

		void SetCanvasStyle(nodegraph::Canvas &canvas) {
			canvas.Look.Background = 0xFF000000;
			canvas.Look.GridFine = 0xFF151515;
			canvas.Look.GridCoarse = 0xFF202020;
			canvas.Look.NodeBody = 0xFF0C0C0C;
			canvas.Look.NodeBorder = 0xFF3A3A3A;
			canvas.Look.NodeSelected = 0xFFFFFFFF;
			canvas.Look.Text = 0xFFFFFFFF;
			canvas.Look.Muted = 0xFFAAAAAA;
			canvas.Look.Widget = 0xFF252525;
			canvas.Look.WidgetFill = 0xFF858585;
		}

		void SyncCanvas(State &state) {
			for (const nodegraph::Node &canvasNode : state.Graph.Nodes()) {
				if (state.Ids.ToDocument.contains(canvasNode.Id) || !canvasNode.DynamicInputs.empty())
					continue;
				const engine::imagegraph::NodeSchema *schema =
					engine::imagegraph::FindSchema(canvasNode.Type);
				if (schema != nullptr && schema->DynamicInputs) {
					(void)state.Graph.SetDynamicInputs(
						canvasNode.Id, {nodegraph::PortSpec{"item-1", "imagegraph.image"}}
					);
				}
			}
			Document updated;
			if (!SaveImageGraphCanvas(state.Graph, state.Authored, state.Ids, updated, state.AdapterError)) {
				RequestPreview(state);
				return;
			}
			std::unordered_set<std::string> liveNodeIds;
			liveNodeIds.reserve(updated.Nodes.size());
			for (const Node &node : updated.Nodes)
				liveNodeIds.insert(node.Id);
			std::erase_if(updated.Outputs, [&](const Output &output) {
				return !liveNodeIds.contains(output.NodeId);
			});
			std::erase_if(updated.Keyframes, [&](const Keyframe &frame) {
				return !liveNodeIds.contains(frame.NodeId);
			});
			state.History.Record(state.Authored, updated);
			if (state.Authored != updated) {
				state.Authored = std::move(updated);
				AuthoredDocumentChanged(state);
				if (std::none_of(
						state.Authored.Outputs.begin(),
						state.Authored.Outputs.end(),
						[&](const Output &output) { return output.Id == state.SelectedOutput; }
					)) {
					state.SelectedOutput =
						state.Authored.Outputs.empty() ? std::string{} : state.Authored.Outputs.front().Id;
				}
			}
		}

		void ReloadCanvas(State &state) {
			const auto issuedNodeIds = state.Ids.IssuedNodeIds;
			const auto issuedGroupIds = state.Ids.IssuedGroupIds;
			const uint64_t nextNodeId = state.Ids.NextNodeId;
			const uint64_t nextGroupId = state.Ids.NextGroupId;
			const std::string selectedDocumentId =
				state.Canvas.Selection().empty()
					? std::string{}
					: (state.Ids.ToDocument.contains(state.Canvas.Selection().front())
						   ? state.Ids.ToDocument.at(state.Canvas.Selection().front())
						   : std::string{});
			if (!LoadImageGraphCanvas(state.Authored, state.Graph, state.Ids, state.AdapterError)) {
				return;
			}
			state.Ids.IssuedNodeIds.insert(issuedNodeIds.begin(), issuedNodeIds.end());
			state.Ids.IssuedGroupIds.insert(issuedGroupIds.begin(), issuedGroupIds.end());
			state.Ids.NextNodeId = std::max(state.Ids.NextNodeId, nextNodeId);
			state.Ids.NextGroupId = std::max(state.Ids.NextGroupId, nextGroupId);
			while (state.Ids.IssuedNodeIds.contains("node-" + std::to_string(state.Ids.NextNodeId))) {
				state.Ids.NextNodeId++;
			}
			while (state.Ids.IssuedGroupIds.contains("group-" + std::to_string(state.Ids.NextGroupId))) {
				state.Ids.NextGroupId++;
			}
			SetCanvasStyle(state.Canvas);
			state.Canvas.Signals.Changed = [&state] { SyncCanvas(state); };
			state.Canvas.Signals.Rerun = [&state](nodegraph::NodeId) { RequestPreview(state, true); };
			if (!selectedDocumentId.empty()) {
				if (const auto selected = state.Ids.ToCanvas.find(selectedDocumentId);
					selected != state.Ids.ToCanvas.end()) {
					state.Canvas.Select(selected->second);
				}
			}
			RequestPreview(state, true);
			if (state.SelectedOutput.empty() ||
				std::none_of(
					state.Authored.Outputs.begin(), state.Authored.Outputs.end(), [&](const Output &output) {
						return output.Id == state.SelectedOutput;
					}
				)) {
				state.SelectedOutput =
					state.Authored.Outputs.empty() ? std::string{} : state.Authored.Outputs.front().Id;
			}
		}

		void Initialize(State &state) {
			if (state.Initialized) return;
			state.Initialized = true;
			studio::RegisterImageGraphNodeTypes();
			Node solid;
			solid.Id = "solid-1";
			solid.Type = "image.solid";
			solid.Values = {
				{"width", int64_t{64}}, {"height", int64_t{64}}, {"colour", Colour{255, 255, 255, 255}}
			};
			state.Authored.Nodes.push_back(std::move(solid));
			state.Authored.Outputs.push_back({"output-main", "solid-1", "image"});
			state.SelectedOutput = "output-main";
			state.NextOutputId = 1;
			ReloadCanvas(state);
		}

		bool
		ReadPxcxFile(const std::filesystem::path &path, std::vector<std::byte> &bytes, std::string &failure) {
			std::error_code filesystemError;
			const uintmax_t size = std::filesystem::file_size(path, filesystemError);
			if (filesystemError) {
				failure = "could not read PXCX file size: " + filesystemError.message();
				return false;
			}
			if (size > engine::bake::PxcxLimits::MaximumArchiveBytes) {
				failure = "PXCX archive exceeds the 64 MiB Studio import limit";
				return false;
			}
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				failure = "could not open PXCX archive";
				return false;
			}
			bytes.resize(static_cast<size_t>(size));
			if (!bytes.empty()) {
				file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (!file || static_cast<size_t>(file.gcount()) != bytes.size()) {
					failure = "PXCX archive changed or ended while it was being read";
					bytes.clear();
					return false;
				}
			}
			return true;
		}

		bool SaveNativeGraph(State &state) {
			const std::string_view graphName(state.GraphName);
			const std::filesystem::path path =
				ImageGraphDocumentPath(engine::core::Paths::Assets(), graphName);
			if (path.empty()) {
				state.GraphIoMessage = "name must be 1 to 255 ASCII letters, digits, hyphens or underscores";
				return false;
			}
			if (std::any_of(state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [](const Node &node) {
					return node.Type.starts_with("pxcx.opaque/");
				})) {
				state.GraphIoMessage = "PXCX nodes remain opaque; save the original archive or remove them "
									   "before native graph save";
				return false;
			}
			const std::string text = engine::imagegraph::Write(state.Authored);
			if (text.size() > IMAGE_GRAPH_FILE_MAXIMUM_BYTES) {
				state.GraphIoMessage = "native graph exceeds the 8 MiB write limit";
				return false;
			}
			std::error_code filesystemError;
			std::filesystem::create_directories(path.parent_path(), filesystemError);
			if (filesystemError) {
				state.GraphIoMessage = "could not create imagegraphs directory: " + filesystemError.message();
				return false;
			}
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file) {
				state.GraphIoMessage = "could not create native graph file";
				return false;
			}
			file.write(text.data(), static_cast<std::streamsize>(text.size()));
			file.flush();
			if (!file) {
				state.GraphIoMessage = "could not finish writing native graph file";
				return false;
			}
			state.GraphIoMessage = "saved " + path.filename().string();
			return true;
		}

		bool OpenNativeGraph(State &state) {
			const std::string_view graphName(state.GraphName);
			const std::filesystem::path path =
				ImageGraphDocumentPath(engine::core::Paths::Assets(), graphName);
			if (path.empty()) {
				state.GraphIoMessage = "name must be 1 to 255 ASCII letters, digits, hyphens or underscores";
				return false;
			}
			Document candidate;
			if (!ReadImageGraphDocument(
					engine::core::Paths::Assets(), graphName, candidate, state.GraphIoMessage
				))
				return false;
			nodegraph::Graph graph;
			ImageGraphCanvasIds ids;
			std::string error;
			if (!LoadImageGraphCanvas(candidate, graph, ids, error)) {
				state.GraphIoMessage = std::move(error);
				return false;
			}
			state.Authored = std::move(candidate);
			state.Playback.CurrentTick = 0;
			ApplyImageGraphTimeline(state.Authored, state.Playback);
			state.History.Clear();
			AuthoredDocumentChanged(state);
			state.ImportedPxcx.reset();
			state.PxcxProjection = {};
			state.PxcxDiagnostics.clear();
			state.PxcxPathDisplay.clear();
			state.PxcxReferenceThumbnail = {};
			state.RetiredPxcxThumbnailTexture = state.CurrentPxcxThumbnailTexture;
			state.CurrentPxcxThumbnailTexture = engine::core::Name{};
			state.PxcxThumbnailTextureHash = 0;
			state.PxcxThumbnailMessage.clear();
			state.AdapterError.clear();
			state.Canvas.Select(nodegraph::NO_NODE);
			ReloadCanvas(state);
			state.GraphIoMessage = "opened " + path.filename().string();
			return true;
		}

		bool OpenAudioCapture(State &state) {
			const std::filesystem::path path(state.AudioCapturePath);
			if (path.empty()) {
				state.AudioCaptureMessage = "enter a recorded audio capture path";
				return false;
			}
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				state.AudioCaptureMessage = "could not open recorded audio capture";
				return false;
			}
			file.seekg(0, std::ios::end);
			const std::streamoff size = file.tellg();
			if (size < 0 ||
				static_cast<uint64_t>(size) > engine::imagegraph::Limits::MaximumAudioCaptureDocumentBytes) {
				state.AudioCaptureMessage = "recorded audio capture exceeds the 8 MiB read limit";
				return false;
			}
			std::string text(static_cast<size_t>(size), '\0');
			file.seekg(0, std::ios::beg);
			if (!text.empty()) {
				file.read(text.data(), static_cast<std::streamsize>(text.size()));
				if (file.gcount() != static_cast<std::streamsize>(text.size())) {
					state.AudioCaptureMessage = "could not read the complete recorded audio capture";
					return false;
				}
			}
			std::vector<engine::imagegraph::AudioCaptureFrame> candidate;
			Diagnostic diagnostic;
			if (engine::imagegraph::ReadAudioCapture(text, candidate, diagnostic) != Status::Ok) {
				state.AudioCaptureMessage = diagnostic.Message;
				state.LastDiagnostic = std::move(diagnostic);
				return false;
			}
			state.AudioFrames = std::move(candidate);
			state.AudioCapturePathDisplay = path.string();
			state.AudioCaptureMessage = "loaded " + std::to_string(state.AudioFrames.size()) +
										" recorded mono frames; preview selects exact source ID and tick";
			state.LastDiagnostic = {};
			RequestPreview(state, true);
			return true;
		}

		bool OpenPxcx(State &state) {
			const std::filesystem::path path(state.PxcxPath);
			if (path.empty()) {
				state.PxcxOpenError = "enter a PXCX archive path";
				return false;
			}
			std::vector<std::byte> bytes;
			if (!ReadPxcxFile(path, bytes, state.PxcxOpenError)) return false;

			engine::bake::PxcxArchive archive;
			if (!engine::bake::ReadPxcx(bytes, archive, state.PxcxOpenError)) return false;
			engine::imagegraphio::PxcxImport imported;
			if (!engine::imagegraphio::ImportPxcxImageGraph(archive, imported, state.PxcxOpenError)) {
				state.ImportedPxcx = std::move(archive);
				state.PxcxPathDisplay = path.string();
				state.PxcxReferenceThumbnail = {};
				state.RetiredPxcxThumbnailTexture = state.CurrentPxcxThumbnailTexture;
				state.CurrentPxcxThumbnailTexture = engine::core::Name{};
				state.PxcxThumbnailTextureHash = 0;
				state.PxcxThumbnailMessage.clear();
				state.PxcxProjection = {};
				state.PxcxDiagnostics.clear();
				return false;
			}
			engine::imagegraph::Diagnostic migrationDiagnostic;
			if (engine::imagegraph::Migrate(imported.Graph, migrationDiagnostic) != Status::Ok) {
				state.PxcxOpenError = migrationDiagnostic.Message.empty()
										  ? "imported image graph cannot be migrated"
										  : migrationDiagnostic.Message;
				return false;
			}

			const std::optional<engine::imagegraphio::PxcxReferencePreview> reference =
				imported.ReferencePreview();
			Image sourceThumbnail;
			if (reference.has_value() &&
				reference->Rgba.size() == engine::bake::PxcxLimits::ThumbnailRgbaBytes) {
				sourceThumbnail = {
					reference->Width,
					reference->Height,
					std::vector<uint8_t>(reference->Rgba.begin(), reference->Rgba.end()),
					reference->Hash
				};
			}
			RegisterPxcxCanvasNodeTypes(imported.Source);
			state.ImportedPxcx = std::move(imported.Source);
			state.PxcxReferenceThumbnail = std::move(sourceThumbnail);
			state.RetiredPxcxThumbnailTexture = state.CurrentPxcxThumbnailTexture;
			state.CurrentPxcxThumbnailTexture = engine::core::Name{};
			state.PxcxThumbnailTextureHash = 0;
			state.PxcxThumbnailMessage.clear();
			state.PxcxPathDisplay = path.string();
			state.PxcxOpenError.clear();
			state.PxcxDiagnostics = std::move(imported.Diagnostics);
			state.PxcxProjection = imported.Graph;
			state.Authored = std::move(imported.Graph);
			state.Playback.CurrentTick = 0;
			ApplyImageGraphTimeline(state.Authored, state.Playback);
			AuthoredDocumentChanged(state);
			state.SelectedOutput =
				state.Authored.Outputs.empty() ? std::string{} : state.Authored.Outputs.front().Id;
			state.NextOutputId = 1;
			state.History.Clear();
			state.RetiredTexture = state.CurrentTexture;
			state.CurrentTexture = engine::core::Name{};
			state.HaveGoodPreview = false;
			state.PreviewHash = 0;
			state.PreviewWidth = 0;
			state.PreviewHeight = 0;
			state.Canvas.Select(nodegraph::NO_NODE);
			ReloadCanvas(state);
			if (state.Graph.Links().size() != state.Authored.Links.size()) {
				state.AdapterError = "one or more positional PXCX links could not be shown on the canvas";
			}
			return true;
		}

		void PumpRetiredTexture(State &state, engine::render::Renderer &renderer) {
			if (state.RetiredTexture.IsValid()) {
				renderer.DropTexture(state.RetiredTexture);
				state.RetiredTexture = engine::core::Name{};
			}
			if (state.RetiredPxcxThumbnailTexture.IsValid()) {
				renderer.DropTexture(state.RetiredPxcxThumbnailTexture);
				state.RetiredPxcxThumbnailTexture = engine::core::Name{};
			}
		}

		bool UploadPreview(State &state, engine::render::Renderer &renderer, const Image &image) {
			if (image.Width == 0 || image.Height == 0 || image.Width > PREVIEW_MAXIMUM_DIMENSION ||
				image.Height > PREVIEW_MAXIMUM_DIMENSION || image.Pixels.size() > PREVIEW_MAXIMUM_BYTES ||
				image.Pixels.size() != static_cast<size_t>(image.Width) * image.Height * 4) {
				state.SinkMessage = "preview exceeds the 128 by 128 texture budget";
				return false;
			}
			if (state.HaveGoodPreview && state.PreviewHash == image.Hash &&
				state.PreviewWidth == image.Width && state.PreviewHeight == image.Height) {
				return true;
			}

			engine::assets::TextureData texture;
			texture.Width = image.Width;
			texture.Height = image.Height;
			texture.Format = engine::assets::TextureFormat::RGBA8;
			texture.Pixels.resize(image.Pixels.size());
			std::memcpy(texture.Pixels.data(), image.Pixels.data(), image.Pixels.size());

			const engine::core::Name name(
				"studio.imagecomposer.preview/" + std::to_string(state.TextureSlot)
			);
			if (!renderer.AddTexture(name, texture)) {
				state.SinkMessage = "renderer refused the preview texture";
				return false;
			}
			const engine::core::Name old = state.CurrentTexture;
			state.CurrentTexture = name;
			state.RetiredTexture = old;
			state.TextureSlot = state.TextureSlot == 1 ? 2 : 1;
			state.PreviewHash = image.Hash;
			state.PreviewWidth = image.Width;
			state.PreviewHeight = image.Height;
			state.HaveGoodPreview = true;
			state.SinkMessage.clear();
			return true;
		}

		bool UploadPxcxReferenceThumbnail(State &state, engine::render::Renderer &renderer) {
			const Image &image = state.PxcxReferenceThumbnail;
			constexpr size_t MaximumBytes = engine::bake::PxcxLimits::ThumbnailRgbaBytes;
			if (image.Width != 256 || image.Height != 256 || image.Pixels.size() != MaximumBytes) {
				state.PxcxThumbnailMessage = "PXCX source thumbnail has invalid RGBA8 dimensions";
				return false;
			}
			if (state.CurrentPxcxThumbnailTexture.IsValid() && state.PxcxThumbnailTextureHash == image.Hash)
				return true;

			engine::assets::TextureData texture;
			texture.Width = image.Width;
			texture.Height = image.Height;
			texture.Format = engine::assets::TextureFormat::RGBA8;
			texture.Pixels.resize(image.Pixels.size());
			std::memcpy(texture.Pixels.data(), image.Pixels.data(), image.Pixels.size());
			const engine::core::Name name(
				"studio.imagecomposer.pxcx-thumbnail/" + std::to_string(state.PxcxThumbnailTextureSlot)
			);
			if (!renderer.AddTexture(name, texture)) {
				state.PxcxThumbnailMessage = "renderer refused the PXCX source thumbnail";
				return false;
			}
			state.RetiredPxcxThumbnailTexture = state.CurrentPxcxThumbnailTexture;
			state.CurrentPxcxThumbnailTexture = name;
			state.PxcxThumbnailTextureSlot = state.PxcxThumbnailTextureSlot == 1 ? 2 : 1;
			state.PxcxThumbnailTextureHash = image.Hash;
			state.PxcxThumbnailMessage.clear();
			return true;
		}

		void RefreshPreview(State &state, engine::render::Renderer &renderer) {
			if (!state.PreviewDirty || (!state.LivePreview && !state.PreviewRequested)) return;
			state.PreviewDirty = false;
			state.PreviewRequested = false;
			state.SinkMessage.clear();
			if (state.SelectedOutput.empty()) {
				state.HaveScalarPreview = false;
				if (state.ImportedPxcx.has_value() && state.Authored == state.PxcxProjection) {
					const std::string nodeId =
						state.PxcxDiagnostics.empty() ? std::string{} : state.PxcxDiagnostics.front().NodeId;
					state.LastDiagnostic = {
						Status::UnsupportedExecution,
						nodeId,
						{},
						"PXCX has no projected image output; the original source archive remains available"
					};
					return;
				}
				state.LastDiagnostic = {
					Status::InvalidOutput, {}, {}, "select or add an image output to preview this graph"
				};
				return;
			}
			const auto selectedOutput = std::find_if(
				state.Authored.Outputs.begin(), state.Authored.Outputs.end(), [&](const Output &output) {
					return output.Id == state.SelectedOutput;
				}
			);
			if (selectedOutput == state.Authored.Outputs.end()) {
				state.HaveScalarPreview = false;
				state.LastDiagnostic = {
					Status::InvalidOutput, {}, {}, "selected output is missing from the authored document"
				};
				return;
			}
			const size_t outputIndex = static_cast<size_t>(selectedOutput - state.Authored.Outputs.begin());
			const Document &previewDocument = state.Authored;
			const Node *outputNode = FindNode(previewDocument, selectedOutput->NodeId);
			const engine::imagegraph::NodeSchema *outputSchema =
				outputNode == nullptr ? nullptr : engine::imagegraph::FindSchema(outputNode->Type);
			const engine::imagegraph::PortSchema *outputPort = nullptr;
			if (outputSchema != nullptr) {
				const auto found = std::find_if(
					outputSchema->Ports.begin(), outputSchema->Ports.end(), [&](const auto &port) {
						return port.Direction == engine::imagegraph::PortDirection::Output &&
							   port.Id == selectedOutput->Port;
					}
				);
				if (found != outputSchema->Ports.end()) outputPort = &*found;
			}
			if (outputPort == nullptr) {
				state.HaveScalarPreview = false;
				state.LastDiagnostic = {
					Status::InvalidOutput,
					selectedOutput->NodeId,
					selectedOutput->Port,
					"selected output port is not declared"
				};
				return;
			}
			const bool imageOutput = outputPort->Type == engine::imagegraph::ValueType::Image;
			if (!imageOutput && outputPort->Type != engine::imagegraph::ValueType::Scalar) {
				state.HaveScalarPreview = false;
				state.LastDiagnostic = {
					Status::UnsupportedExecution,
					selectedOutput->NodeId,
					selectedOutput->Port,
					"Studio preview supports image and scalar outputs"
				};
				return;
			}
			if (imageOutput) {
				state.HaveScalarPreview = false;
				if (const Image *cached = state.PreviewCache.Find(
						state.DocumentRevision,
						outputIndex,
						state.Playback.CurrentTick,
						state.Playback.Subframe
					)) {
					if (!UploadPreview(state, renderer, *cached)) return;
					state.Preview = *cached;
					state.LastDiagnostic = {};
					return;
				}
			} else {
				state.HaveGoodPreview = false;
				state.HaveScalarPreview = false;
			}
			engine::imagegraph::Plan plan;
			Diagnostic diagnostic;
			if (imageOutput && !CheckImageComposerPreviewBudget(previewDocument, diagnostic)) {
				state.LastDiagnostic = std::move(diagnostic);
				return;
			}
			{
				ENGINE_PROFILE_CAT("image composer preview", engine::core::ProfileCategory::Render);
				if (engine::imagegraph::Compile(previewDocument, plan, diagnostic) != Status::Ok) {
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				engine::imagegraph::EvaluationRequest request;
				request.Tick = state.Playback.CurrentTick;
				request.Subframe = state.Playback.Subframe;
				request.AudioFrames =
					std::span<const engine::imagegraph::AudioCaptureFrame>(state.AudioFrames);
				studio::ImageGraphPreviewValue preview;
				if (studio::EvaluateImageGraphPreview(
						previewDocument, plan, state.SelectedOutput, request, preview, diagnostic
					) != Status::Ok) {
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				if (const auto *value = std::get_if<engine::imagegraph::EvaluatedValue>(&preview)) {
					const auto *scalar = std::get_if<double>(&value->Data);
					if (scalar == nullptr) {
						state.LastDiagnostic = {
							Status::InvalidOutput,
							selectedOutput->NodeId,
							selectedOutput->Port,
							"scalar output did not evaluate to a finite numeric value"
						};
						return;
					}
					state.ScalarPreviewValue = *scalar;
					state.ScalarPreviewPort = value->Port;
					state.HaveScalarPreview = true;
					state.LastDiagnostic = {};
					return;
				}
				const auto *image = std::get_if<Image>(&preview);
				if (image == nullptr) {
					state.LastDiagnostic = {
						Status::InvalidOutput,
						selectedOutput->NodeId,
						selectedOutput->Port,
						"image output did not produce pixels"
					};
					return;
				}
				if (!UploadPreview(state, renderer, *image)) return;
				(void)state.PreviewCache.Store(
					state.DocumentRevision,
					outputIndex,
					state.Playback.CurrentTick,
					*image,
					state.Playback.Subframe
				);
				state.Preview = *image;
				state.LastDiagnostic = {};
			}
		}

		void ApplyHistory(State &state, bool redo) {
			const bool changed =
				redo ? state.History.Redo(state.Authored) : state.History.Undo(state.Authored);
			if (changed) {
				ApplyImageGraphTimeline(state.Authored, state.Playback);
				AuthoredDocumentChanged(state);
				ReloadCanvas(state);
			}
		}

		void ApplyDocumentEdit(State &state, const auto &edit) {
			const Document before = state.Authored;
			edit(state.Authored);
			if (state.Authored == before) return;
			state.History.Record(before, state.Authored);
			AuthoredDocumentChanged(state);
		}

		engine::imagegraph::TimelineSettings PlaybackTimeline(const ImageGraphPlayback &playback) {
			return {
				playback.TotalFrames,
				playback.StartTick,
				playback.EndTick,
				playback.PingPong ? "pingpong"
				: playback.Loop	  ? "loop"
								  : "stop",
				playback.FramesPerSecond
			};
		}

		bool SavePlaybackTimeline(State &state) {
			bool saved = false;
			ApplyDocumentEdit(state, [&](Document &document) {
				if (studio::SetImageGraphTimeline(
						document, PlaybackTimeline(state.Playback), state.LastDiagnostic
					)) {
					state.LastDiagnostic = {};
					saved = true;
				}
			});
			if (!saved && state.Authored.Timeline) ApplyImageGraphTimeline(state.Authored, state.Playback);
			return saved;
		}

		bool RemoveSavedTimeline(State &state) {
			bool removed = false;
			ApplyDocumentEdit(state, [&](Document &document) {
				if (studio::RemoveImageGraphTimeline(document, state.LastDiagnostic)) {
					state.LastDiagnostic = {};
					removed = true;
				}
			});
			return removed;
		}

		void SavePlaybackIfAuthored(State &state) {
			if (state.Authored.Timeline) (void)SavePlaybackTimeline(state);
		}

		std::string SelectedNodeId(const State &state) {
			if (state.Canvas.Selection().empty()) return {};
			const auto found = state.Ids.ToDocument.find(state.Canvas.Selection().front());
			return found == state.Ids.ToDocument.end() ? std::string{} : found->second;
		}

		void DrawToolbar(State &state) {
			ImGui::BeginDisabled(!state.History.CanUndo());
			if (ImGui::Button("Undo")) ApplyHistory(state, false);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!state.History.CanRedo());
			if (ImGui::Button("Redo")) ApplyHistory(state, true);
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("New graph")) {
				const Document before = state.Authored;
				Document fresh;
				fresh.Nodes.push_back(
					{"solid-1",
					 "image.solid",
					 "",
					 {},
					 {{"width", int64_t{64}},
					  {"height", int64_t{64}},
					  {"colour", Colour{255, 255, 255, 255}}},
					 {}}
				);
				fresh.Outputs.push_back({"output-main", "solid-1", "image"});
				state.Authored = std::move(fresh);
				state.History.Record(before, state.Authored);
				AuthoredDocumentChanged(state);
				state.SelectedOutput = "output-main";
				state.NextOutputId = 1;
				state.Playback = {};
				ApplyImageGraphTimeline(state.Authored, state.Playback);
				state.Canvas.Select(nodegraph::NO_NODE);
				ReloadCanvas(state);
			}
			ImGui::SameLine();
			ImGui::Checkbox("Live preview", &state.LivePreview);
			ImGui::SameLine();
			if (ImGui::Button("Render")) {
				RequestPreview(state, true);
			}
			ImGui::SameLine();
			ImGui::Text("tick %llu", static_cast<unsigned long long>(state.Playback.CurrentTick));
		}

		void DrawPalette(State &state) {
			ImGui::InputTextWithHint(
				"##image-node-search", "Search nodes", state.Search, sizeof(state.Search)
			);
			ImGui::Separator();
			const std::string_view search(state.Search);
			for (const nodegraph::NodeType &type : nodegraph::NodeTypes::All()) {
				if (engine::imagegraph::FindSchema(type.Id) == nullptr) continue;
				if (!search.empty() && type.Title.find(search) == std::string::npos &&
					type.Id.find(search) == std::string::npos) {
					continue;
				}
				ImGui::PushID(type.Id.c_str());
				if (ImGui::Selectable(type.Title.c_str())) {
					const float offset = static_cast<float>(state.Graph.Nodes().size()) * 40.0f;
					const nodegraph::NodeId node =
						state.Graph.Add(type.Id, 40.0f + offset, 40.0f + offset * 0.2f);
					if (node != nodegraph::NO_NODE) {
						state.Canvas.Select(node);
						state.Canvas.Centre(state.Graph, node);
						SyncCanvas(state);
					}
				}
				ImGui::PopID();
			}
		}

		void BeginPropertyEdit(State &state, ImGuiID id) {
			if (ImGui::IsItemActivated()) {
				state.HaveActiveEdit = true;
				state.ActiveEditId = id;
				state.EditBefore = state.Authored;
			}
		}

		void EndPropertyEdit(State &state, ImGuiID id, bool changed) {
			if (changed) {
				AuthoredDocumentChanged(state);
				state.PreviewRequested = false;
			}
			if (!state.HaveActiveEdit && changed) {
				state.EditBefore = state.Authored;
				state.HaveActiveEdit = true;
				state.ActiveEditId = id;
			}
			if (ImGui::IsItemDeactivatedAfterEdit() && state.HaveActiveEdit && state.ActiveEditId == id) {
				state.History.Record(state.EditBefore, state.Authored);
				state.HaveActiveEdit = false;
				state.ActiveEditId = 0;
			}
		}

		void FinishInactiveEdit(State &state) {
			if (!state.HaveActiveEdit || GImGui->ActiveId == state.ActiveEditId) return;
			state.History.Record(state.EditBefore, state.Authored);
			state.HaveActiveEdit = false;
			state.ActiveEditId = 0;
		}

		int ResizeTextInput(ImGuiInputTextCallbackData *data) {
			if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) return 0;
			auto *text = static_cast<std::string *>(data->UserData);
			text->resize(static_cast<size_t>(data->BufTextLen));
			data->Buf = text->data();
			return 0;
		}

		bool DrawColourField(const char *label, Colour &colour) {
			float edit[]{
				static_cast<float>(colour.Red) / 255.0f,
				static_cast<float>(colour.Green) / 255.0f,
				static_cast<float>(colour.Blue) / 255.0f,
				static_cast<float>(colour.Alpha) / 255.0f
			};
			if (!ImGui::ColorEdit4(label, edit, ImGuiColorEditFlags_NoInputs)) return false;
			const auto byte = [](float channel) {
				return static_cast<uint8_t>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f);
			};
			colour = Colour{byte(edit[0]), byte(edit[1]), byte(edit[2]), byte(edit[3])};
			return true;
		}

		bool DrawByteField(const char *label, uint8_t &value, int maximum) {
			int edit = value;
			if (!ImGui::InputInt(label, &edit, 1, 1)) return false;
			value = static_cast<uint8_t>(std::clamp(edit, 0, maximum));
			return true;
		}

		bool DrawGradientValue(engine::imagegraph::Gradient &gradient) {
			bool changed = DrawByteField("Mode", gradient.Mode, 6);
			for (size_t index = 0; index < gradient.Keys.size(); index++) {
				auto &key = gradient.Keys[index];
				ImGui::PushID(static_cast<int>(index));
				changed = ImGui::InputDouble("Time", &key.Time, 0.01, 0.1, "%.4f") || changed;
				ImGui::SameLine();
				changed = DrawColourField("Colour", key.Color) || changed;
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					gradient.Keys.erase(gradient.Keys.begin() + static_cast<std::ptrdiff_t>(index));
					changed = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::BeginDisabled(gradient.Keys.size() >= engine::imagegraph::Limits::MaximumGradientKeys);
			if (ImGui::SmallButton("Add key") &&
				gradient.Keys.size() < engine::imagegraph::Limits::MaximumGradientKeys) {
				gradient.Keys.push_back({gradient.Keys.empty() ? 0.0 : gradient.Keys.back().Time, Colour{}});
				changed = true;
			}
			ImGui::EndDisabled();
			return changed;
		}

		bool DrawAreaValue(engine::imagegraph::Area &area) {
			double bounds[]{area.CenterX, area.CenterY, area.HalfWidth, area.HalfHeight};
			bool changed = ImGui::InputScalarN(
				"Center / half size", ImGuiDataType_Double, bounds, 4, nullptr, nullptr, "%.4f"
			);
			if (changed) {
				area.CenterX = bounds[0];
				area.CenterY = bounds[1];
				area.HalfWidth = bounds[2];
				area.HalfHeight = bounds[3];
			}
			changed = DrawByteField("Shape", area.Shape, 1) || changed;
			changed = DrawByteField("Mode", area.Mode, 2) || changed;
			return changed;
		}

		bool DrawCurveValue(engine::imagegraph::Curve &curve) {
			bool changed = ImGui::InputScalarN(
				"Header", ImGuiDataType_Double, curve.Header.data(), 6, nullptr, nullptr, "%.4f"
			);
			for (size_t index = 0; index < curve.Anchors.size(); index++) {
				ImGui::PushID(static_cast<int>(index));
				changed = ImGui::InputScalarN(
							  "Anchor",
							  ImGuiDataType_Double,
							  curve.Anchors[index].data(),
							  6,
							  nullptr,
							  nullptr,
							  "%.4f"
						  ) ||
						  changed;
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					curve.Anchors.erase(curve.Anchors.begin() + static_cast<std::ptrdiff_t>(index));
					changed = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::BeginDisabled(curve.Anchors.size() >= engine::imagegraph::Limits::MaximumCurveAnchors);
			if (ImGui::SmallButton("Add anchor") &&
				curve.Anchors.size() < engine::imagegraph::Limits::MaximumCurveAnchors) {
				curve.Anchors.emplace_back();
				changed = true;
			}
			ImGui::EndDisabled();
			return changed;
		}

		bool DrawVector4Value(engine::imagegraph::Vector4 &vector) {
			double fields[]{vector.X, vector.Y, vector.Z, vector.W};
			if (!ImGui::InputScalarN("Components", ImGuiDataType_Double, fields, 4, nullptr, nullptr, "%.4f"))
				return false;
			vector = {fields[0], fields[1], fields[2], fields[3]};
			return true;
		}

		bool DrawVector3Value(engine::imagegraph::Vector3 &vector) {
			double fields[]{vector.X, vector.Y, vector.Z};
			if (!ImGui::InputScalarN("Components", ImGuiDataType_Double, fields, 3, nullptr, nullptr, "%.4f"))
				return false;
			vector = {fields[0], fields[1], fields[2]};
			return true;
		}

		bool DrawQuaternionValue(engine::imagegraph::Quaternion &rotation) {
			double fields[]{rotation.X, rotation.Y, rotation.Z, rotation.W};
			if (!ImGui::InputScalarN("Components", ImGuiDataType_Double, fields, 4, nullptr, nullptr, "%.4f"))
				return false;
			rotation = {fields[0], fields[1], fields[2], fields[3]};
			return true;
		}

		bool DrawPathValue(State &state, engine::imagegraph::Path2D &path) {
			bool changed = ImGui::Checkbox("Loop", &path.Loop);
			constexpr size_t pageSize = 32;
			const size_t lastPage =
				path.Anchors.empty() ? 0 : ((path.Anchors.size() - 1) / pageSize) * pageSize;
			size_t &page = state.ArrayPageOffset;
			page = std::min(page, lastPage);
			if (!path.Anchors.empty()) {
				ImGui::SameLine();
				ImGui::BeginDisabled(page == 0);
				if (ImGui::SmallButton("Prev anchors")) page -= pageSize;
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::Text("%zu / %zu", page + 1, path.Anchors.size());
				ImGui::SameLine();
				ImGui::BeginDisabled(page >= lastPage);
				if (ImGui::SmallButton("Next anchors")) page += pageSize;
				ImGui::EndDisabled();
			}
			const size_t end = std::min(path.Anchors.size(), page + pageSize);
			for (size_t index = page; index < end; index++) {
				auto &anchor = path.Anchors[index];
				ImGui::PushID(static_cast<int>(index));
				changed = ImGui::InputScalar("Index", ImGuiDataType_S64, &anchor.Index) || changed;
				changed =
					ImGui::InputScalarN(
						"Controls", ImGuiDataType_Double, anchor.Controls.data(), 6, nullptr, nullptr, "%.4f"
					) ||
					changed;
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					path.Anchors.erase(path.Anchors.begin() + static_cast<std::ptrdiff_t>(index));
					changed = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::BeginDisabled(path.Anchors.size() >= engine::imagegraph::Limits::MaximumPathAnchors);
			if (ImGui::SmallButton("Add path anchor") &&
				path.Anchors.size() < engine::imagegraph::Limits::MaximumPathAnchors) {
				path.Anchors.emplace_back();
				changed = true;
			}
			ImGui::EndDisabled();

			for (size_t index = 0; index < path.Weights.size(); index++) {
				auto &weight = path.Weights[index];
				ImGui::PushID(static_cast<int>(index));
				double fields[]{weight.Position, weight.Weight};
				const bool edit = ImGui::InputScalarN(
					"Position / weight", ImGuiDataType_Double, fields, 2, nullptr, nullptr, "%.4f"
				);
				if (edit) {
					weight = {fields[0], fields[1]};
					changed = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove weight")) {
					path.Weights.erase(path.Weights.begin() + static_cast<std::ptrdiff_t>(index));
					changed = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::BeginDisabled(path.Weights.size() >= engine::imagegraph::Limits::MaximumPathWeights);
			if (ImGui::SmallButton("Add path weight") &&
				path.Weights.size() < engine::imagegraph::Limits::MaximumPathWeights) {
				path.Weights.push_back({0.0, 1.0});
				changed = true;
			}
			ImGui::EndDisabled();
			return changed;
		}

		bool DrawArrayValue(State &state, engine::imagegraph::ArrayValue &array, bool colourPalette = false) {
			using engine::imagegraph::ElementValue;
			using engine::imagegraph::ValueType;
			static constexpr ValueType elementTypes[] = {
				ValueType::Boolean,
				ValueType::Integer,
				ValueType::Scalar,
				ValueType::Text,
				ValueType::Colour,
				ValueType::Vector2
			};
			static constexpr const char *elementNames[] = {
				"Boolean", "Integer", "Scalar", "Text", "Colour", "Vector2"
			};
			bool changed = false;
			const size_t maximumElements = colourPalette ? engine::imagegraph::Limits::MaximumPaletteEntries
														 : engine::imagegraph::Limits::MaximumArrayElements;
			if (colourPalette) {
				ImGui::TextUnformatted("Colour palette");
				if (array.ElementType != ValueType::Colour) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Reset palette")) {
						array.ElementType = ValueType::Colour;
						array.Elements = {Colour{0, 0, 0, 255}};
						state.ArrayPageOffset = 0;
						changed = true;
					}
					ImGui::TextDisabled("Expected Array<Colour>");
					return changed;
				}
			} else {
				const auto typeIndex = [&] {
					for (size_t index = 0; index < std::size(elementTypes); index++)
						if (elementTypes[index] == array.ElementType) return index;
					return size_t{0};
				}();
				if (ImGui::BeginCombo("Element type", elementNames[typeIndex])) {
					for (size_t index = 0; index < std::size(elementTypes); index++) {
						if (ImGui::Selectable(elementNames[index], index == typeIndex)) {
							array.ElementType = elementTypes[index];
							array.Elements.clear();
							state.ArrayPageOffset = 0;
							changed = true;
						}
						if (index == typeIndex) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(array.Elements.size() >= maximumElements);
			if (ImGui::SmallButton(colourPalette ? "Add swatch" : "Add element") &&
				array.Elements.size() < maximumElements) {
				switch (array.ElementType) {
				case ValueType::Boolean:
					array.Elements.emplace_back(false);
					break;
				case ValueType::Integer:
					array.Elements.emplace_back(int64_t{0});
					break;
				case ValueType::Scalar:
					array.Elements.emplace_back(0.0);
					break;
				case ValueType::Text:
					array.Elements.emplace_back(std::string{});
					break;
				case ValueType::Colour:
					array.Elements.emplace_back(colourPalette ? Colour{0, 0, 0, 255} : Colour{});
					break;
				case ValueType::Vector2:
					array.Elements.emplace_back(engine::imagegraph::Vector2{});
					break;
				case ValueType::Image:
				case ValueType::Array:
				case ValueType::Gradient:
				case ValueType::Area:
				case ValueType::Curve:
				case ValueType::Vector4:
				case ValueType::Path2D:
				case ValueType::Vector3:
				case ValueType::Quaternion:
				case ValueType::Enum:
				case ValueType::Mesh:
				case ValueType::AudioBit:
					break;
				}
				changed = true;
			}
			ImGui::EndDisabled();
			constexpr size_t pageSize = 32;
			if (!array.Elements.empty()) {
				const size_t lastPage = ((array.Elements.size() - 1) / pageSize) * pageSize;
				state.ArrayPageOffset = std::min(state.ArrayPageOffset, lastPage);
				ImGui::SameLine();
				ImGui::BeginDisabled(state.ArrayPageOffset == 0);
				if (ImGui::SmallButton("Prev 32")) state.ArrayPageOffset -= pageSize;
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::Text("%zu / %zu", state.ArrayPageOffset + 1, array.Elements.size());
				ImGui::SameLine();
				ImGui::BeginDisabled(state.ArrayPageOffset >= lastPage);
				if (ImGui::SmallButton("Next 32")) state.ArrayPageOffset += pageSize;
				ImGui::EndDisabled();
			}
			const size_t end = std::min(array.Elements.size(), state.ArrayPageOffset + pageSize);
			for (size_t index = state.ArrayPageOffset; index < end; index++) {
				ImGui::PushID(static_cast<int>(index));
				ImGui::Text("%zu", index);
				ImGui::SameLine(36.0f);
				ElementValue &element = array.Elements[index];
				bool elementChanged = false;
				if (auto *value = std::get_if<bool>(&element)) {
					elementChanged = ImGui::Checkbox("##element", value);
				} else if (auto *value = std::get_if<int64_t>(&element)) {
					elementChanged = ImGui::InputScalar("##element", ImGuiDataType_S64, value);
				} else if (auto *value = std::get_if<double>(&element)) {
					elementChanged = ImGui::InputDouble("##element", value, 0.01, 1.0, "%.4f");
				} else if (auto *value = std::get_if<std::string>(&element)) {
					elementChanged = ImGui::InputText(
						"##element",
						value->data(),
						value->capacity() + 1,
						ImGuiInputTextFlags_CallbackResize,
						ResizeTextInput,
						value
					);
				} else if (auto *value = std::get_if<Colour>(&element)) {
					float colour[]{
						static_cast<float>(value->Red) / 255.0f,
						static_cast<float>(value->Green) / 255.0f,
						static_cast<float>(value->Blue) / 255.0f,
						static_cast<float>(value->Alpha) / 255.0f
					};
					elementChanged = ImGui::ColorEdit4("##element", colour, ImGuiColorEditFlags_NoInputs);
					if (elementChanged) {
						const auto byte = [](float channel) {
							return static_cast<uint8_t>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f);
						};
						*value = Colour{byte(colour[0]), byte(colour[1]), byte(colour[2]), byte(colour[3])};
					}
				} else if (auto *value = std::get_if<engine::imagegraph::Vector2>(&element)) {
					double coordinates[]{value->X, value->Y};
					elementChanged = ImGui::InputScalarN(
						"##element", ImGuiDataType_Double, coordinates, 2, nullptr, nullptr, "%.4f"
					);
					if (elementChanged) *value = {coordinates[0], coordinates[1]};
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(colourPalette && array.Elements.size() <= 1);
				const bool remove = ImGui::SmallButton("Remove");
				ImGui::EndDisabled();
				if (remove && (!colourPalette || array.Elements.size() > 1)) {
					array.Elements.erase(array.Elements.begin() + static_cast<std::ptrdiff_t>(index));
					changed = true;
					ImGui::PopID();
					break;
				}
				changed = changed || elementChanged;
				ImGui::PopID();
			}
			return changed;
		}

		bool DrawValueWidget(State &state, std::string_view nodeId, AuthoredValue &property) {
			bool changed = false;
			Value replacement = property.Data;
			if (const auto *value = std::get_if<bool>(&property.Data)) {
				bool edit = *value;
				changed = ImGui::Checkbox("##value", &edit);
				if (changed) replacement = edit;
			} else if (const auto *value = std::get_if<int64_t>(&property.Data)) {
				int64_t edit = *value;
				changed = ImGui::InputScalar("##value", ImGuiDataType_S64, &edit);
				if (changed) replacement = edit;
			} else if (const auto *value = std::get_if<double>(&property.Data)) {
				double edit = *value;
				changed = ImGui::InputDouble("##value", &edit, 0.01, 1.0, "%.4f");
				if (changed) replacement = edit;
			} else if (const auto *value = std::get_if<std::string>(&property.Data)) {
				std::string edit = *value;
				changed = ImGui::InputText(
					"##value",
					edit.data(),
					edit.capacity() + 1,
					ImGuiInputTextFlags_CallbackResize,
					ResizeTextInput,
					&edit
				);
				if (changed) replacement = std::move(edit);
			} else if (const auto *value = std::get_if<Colour>(&property.Data)) {
				float edit[]{
					static_cast<float>(value->Red) / 255.0f,
					static_cast<float>(value->Green) / 255.0f,
					static_cast<float>(value->Blue) / 255.0f,
					static_cast<float>(value->Alpha) / 255.0f
				};
				changed = ImGui::ColorEdit4("##value", edit, ImGuiColorEditFlags_NoInputs);
				if (changed) {
					const auto byte = [](float channel) {
						return static_cast<uint8_t>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f);
					};
					replacement = Colour{byte(edit[0]), byte(edit[1]), byte(edit[2]), byte(edit[3])};
				}
			} else if (const auto *value = std::get_if<engine::imagegraph::Vector2>(&property.Data)) {
				double edit[]{value->X, value->Y};
				changed =
					ImGui::InputScalarN("##value", ImGuiDataType_Double, edit, 2, nullptr, nullptr, "%.4f");
				if (changed) replacement = engine::imagegraph::Vector2{edit[0], edit[1]};
			} else if (auto *vector = std::get_if<engine::imagegraph::Vector3>(&replacement)) {
				changed = DrawVector3Value(*vector);
			} else if (auto *rotation = std::get_if<engine::imagegraph::Quaternion>(&replacement)) {
				changed = DrawQuaternionValue(*rotation);
			} else if (auto *choice = std::get_if<engine::imagegraph::EnumValue>(&replacement)) {
				const Node *node = FindNode(state.Authored, nodeId);
				if (node != nullptr && node->Type == "image.transform_3d" && property.Port == "projection") {
					const char *selected = choice->Value == 0	? "Perspective"
										   : choice->Value == 1 ? "Orthographic"
																: "Unknown";
					if (ImGui::BeginCombo("##value", selected)) {
						for (const auto &[value, label] :
							 {std::pair<int64_t, const char *>{0, "Perspective"}, {1, "Orthographic"}}) {
							const bool active = choice->Value == value;
							if (ImGui::Selectable(label, active)) {
								choice->Value = value;
								changed = true;
							}
							if (active) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
				} else {
					changed = ImGui::InputScalar("##value", ImGuiDataType_S64, &choice->Value);
				}
			} else if (auto *array = std::get_if<engine::imagegraph::ArrayValue>(&replacement)) {
				const Node *node = FindNode(state.Authored, nodeId);
				const bool colourPalette =
					node != nullptr && node->Type == "image.posterize" && property.Port == "palette";
				changed = DrawArrayValue(state, *array, colourPalette);
			} else if (auto *gradient = std::get_if<engine::imagegraph::Gradient>(&replacement)) {
				changed = DrawGradientValue(*gradient);
			} else if (auto *area = std::get_if<engine::imagegraph::Area>(&replacement)) {
				changed = DrawAreaValue(*area);
			} else if (auto *curve = std::get_if<engine::imagegraph::Curve>(&replacement)) {
				changed = DrawCurveValue(*curve);
			} else if (auto *vector = std::get_if<engine::imagegraph::Vector4>(&replacement)) {
				changed = DrawVector4Value(*vector);
			} else if (auto *path = std::get_if<engine::imagegraph::Path2D>(&replacement)) {
				changed = DrawPathValue(state, *path);
			} else {
				ImGui::TextDisabled("unsupported authored value");
			}

			const ImGuiID itemId = ImGui::GetID("##value");
			BeginPropertyEdit(state, itemId);
			if (changed &&
				SetImageGraphValue(
					state.Authored, nodeId, property.Port, std::move(replacement), state.LastDiagnostic
				)) {
				state.LastDiagnostic = {};
			}
			EndPropertyEdit(state, itemId, changed);
			return changed;
		}

		const char *ValueTypeName(engine::imagegraph::ValueType type) {
			using engine::imagegraph::ValueType;
			switch (type) {
			case ValueType::Boolean:
				return "Boolean";
			case ValueType::Integer:
				return "Integer";
			case ValueType::Scalar:
				return "Scalar";
			case ValueType::Text:
				return "Text";
			case ValueType::Colour:
				return "Colour";
			case ValueType::Vector2:
				return "Vector2";
			case ValueType::Image:
				return "Image";
			case ValueType::Array:
				return "Array";
			case ValueType::Gradient:
				return "Gradient";
			case ValueType::Area:
				return "Area";
			case ValueType::Curve:
				return "Curve";
			case ValueType::Vector4:
				return "Vector4";
			case ValueType::Path2D:
				return "Path2D";
			case ValueType::Vector3:
				return "Vector3";
			case ValueType::Quaternion:
				return "Quaternion";
			case ValueType::Enum:
				return "Enum";
			case ValueType::Mesh:
				return "Mesh";
			case ValueType::AudioBit:
				return "AudioBit";
			}
			return "Unknown";
		}

		std::optional<Value> DefaultForType(engine::imagegraph::ValueType type) {
			using engine::imagegraph::ValueType;
			switch (type) {
			case ValueType::Boolean:
				return Value{false};
			case ValueType::Integer:
				return Value{int64_t{0}};
			case ValueType::Scalar:
				return Value{0.0};
			case ValueType::Text:
				return Value{std::string{}};
			case ValueType::Colour:
				return Value{Colour{}};
			case ValueType::Vector2:
				return Value{engine::imagegraph::Vector2{}};
			case ValueType::Array:
				return Value{engine::imagegraph::ArrayValue{}};
			case ValueType::Gradient:
				return Value{engine::imagegraph::Gradient{
					0, {{0.0, Colour{0, 0, 0, 255}}, {1.0, Colour{255, 255, 255, 255}}}
				}};
			case ValueType::Area:
				return Value{engine::imagegraph::Area{}};
			case ValueType::Curve:
				return Value{engine::imagegraph::Curve{{}, {{}, {}}}};
			case ValueType::Vector4:
				return Value{engine::imagegraph::Vector4{}};
			case ValueType::Path2D:
				return Value{engine::imagegraph::Path2D{}};
			case ValueType::Vector3:
				return Value{engine::imagegraph::Vector3{}};
			case ValueType::Quaternion:
				return Value{engine::imagegraph::Quaternion{}};
			case ValueType::Enum:
				return Value{engine::imagegraph::EnumValue{}};
			case ValueType::Image:
			case ValueType::Mesh:
			case ValueType::AudioBit:
				return std::nullopt;
			}
			return std::nullopt;
		}

		bool DrawDynamicDefault(State &state, Value &value) {
			bool changed = false;
			if (auto *item = std::get_if<bool>(&value)) {
				changed = ImGui::Checkbox("##default", item);
			} else if (auto *item = std::get_if<int64_t>(&value)) {
				changed = ImGui::InputScalar("##default", ImGuiDataType_S64, item);
			} else if (auto *item = std::get_if<double>(&value)) {
				changed = ImGui::InputDouble("##default", item, 0.01, 1.0, "%.4f");
			} else if (auto *item = std::get_if<std::string>(&value)) {
				changed = ImGui::InputText(
					"##default",
					item->data(),
					item->capacity() + 1,
					ImGuiInputTextFlags_CallbackResize,
					ResizeTextInput,
					item
				);
			} else if (auto *item = std::get_if<Colour>(&value)) {
				float colour[]{
					static_cast<float>(item->Red) / 255.0f,
					static_cast<float>(item->Green) / 255.0f,
					static_cast<float>(item->Blue) / 255.0f,
					static_cast<float>(item->Alpha) / 255.0f
				};
				changed = ImGui::ColorEdit4("##default", colour, ImGuiColorEditFlags_NoInputs);
				if (changed) {
					const auto byte = [](float channel) {
						return static_cast<uint8_t>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f);
					};
					*item = Colour{byte(colour[0]), byte(colour[1]), byte(colour[2]), byte(colour[3])};
				}
			} else if (auto *item = std::get_if<engine::imagegraph::Vector2>(&value)) {
				double coordinates[]{item->X, item->Y};
				changed = ImGui::InputScalarN(
					"##default", ImGuiDataType_Double, coordinates, 2, nullptr, nullptr, "%.4f"
				);
				if (changed) *item = {coordinates[0], coordinates[1]};
			} else if (auto *item = std::get_if<engine::imagegraph::Vector3>(&value)) {
				changed = DrawVector3Value(*item);
			} else if (auto *item = std::get_if<engine::imagegraph::Quaternion>(&value)) {
				changed = DrawQuaternionValue(*item);
			} else if (auto *item = std::get_if<engine::imagegraph::EnumValue>(&value)) {
				changed = ImGui::InputScalar("##default", ImGuiDataType_S64, &item->Value);
			} else if (auto *item = std::get_if<engine::imagegraph::ArrayValue>(&value)) {
				changed = DrawArrayValue(state, *item);
			} else if (auto *item = std::get_if<engine::imagegraph::Gradient>(&value)) {
				changed = DrawGradientValue(*item);
			} else if (auto *item = std::get_if<engine::imagegraph::Area>(&value)) {
				changed = DrawAreaValue(*item);
			} else if (auto *item = std::get_if<engine::imagegraph::Curve>(&value)) {
				changed = DrawCurveValue(*item);
			} else if (auto *item = std::get_if<engine::imagegraph::Vector4>(&value)) {
				changed = DrawVector4Value(*item);
			} else if (auto *item = std::get_if<engine::imagegraph::Path2D>(&value)) {
				changed = DrawPathValue(state, *item);
			} else {
				ImGui::TextDisabled("Image values have no inline default.");
			}
			return changed;
		}

		void DrawDynamicInputs(State &state, Node &node, const engine::imagegraph::NodeSchema &schema) {
			if (!schema.DynamicInputs) return;
			ImGui::Separator();
			ImGui::TextUnformatted("Instance inputs");
			const std::vector<engine::imagegraph::DynamicInput> inputs = node.DynamicInputs;
			for (const auto &source : inputs) {
				auto input = source;
				ImGui::PushID(input.Id.c_str());
				ImGui::TextUnformatted(input.Id.c_str());
				ImGui::SameLine(92.0f);
				static constexpr engine::imagegraph::ValueType types[] = {
					engine::imagegraph::ValueType::Boolean,
					engine::imagegraph::ValueType::Integer,
					engine::imagegraph::ValueType::Scalar,
					engine::imagegraph::ValueType::Text,
					engine::imagegraph::ValueType::Colour,
					engine::imagegraph::ValueType::Vector2,
					engine::imagegraph::ValueType::Image,
					engine::imagegraph::ValueType::Array,
					engine::imagegraph::ValueType::Gradient,
					engine::imagegraph::ValueType::Area,
					engine::imagegraph::ValueType::Curve,
					engine::imagegraph::ValueType::Vector4,
					engine::imagegraph::ValueType::Path2D,
					engine::imagegraph::ValueType::Vector3,
					engine::imagegraph::ValueType::Quaternion,
					engine::imagegraph::ValueType::Enum
				};
				bool changed = false;
				if (ImGui::BeginCombo("##type", ValueTypeName(input.Type))) {
					for (const auto type : types) {
						const bool selected = input.Type == type;
						if (ImGui::Selectable(ValueTypeName(type), selected)) {
							input.Type = type;
							input.Default = DefaultForType(type);
							changed = true;
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				if (input.Type != engine::imagegraph::ValueType::Image) {
					bool hasDefault = input.Default.has_value();
					if (ImGui::Checkbox("Default", &hasDefault)) {
						input.Default = hasDefault ? DefaultForType(input.Type) : std::nullopt;
						changed = true;
					}
					if (input.Default) changed = DrawDynamicDefault(state, *input.Default) || changed;
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(node.DynamicInputs.size() <= 1);
				const bool remove = ImGui::SmallButton("Remove");
				ImGui::EndDisabled();
				if (changed) {
					const std::string nodeId = node.Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						if (SetImageGraphDynamicInput(document, nodeId, input, state.LastDiagnostic))
							state.LastDiagnostic = {};
					});
					ReloadCanvas(state);
				}
				if (remove) {
					const std::string nodeId = node.Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						if (RemoveImageGraphDynamicInput(document, nodeId, input.Id, state.LastDiagnostic))
							state.LastDiagnostic = {};
					});
					ReloadCanvas(state);
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::BeginDisabled(
				node.DynamicInputs.size() >= engine::imagegraph::Limits::MaximumDynamicInputsPerNode
			);
			if (ImGui::SmallButton("Add input") &&
				node.DynamicInputs.size() < engine::imagegraph::Limits::MaximumDynamicInputsPerNode) {
				std::string id;
				for (size_t index = 1;; index++) {
					id = "item-" + std::to_string(index);
					if (std::none_of(
							node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const auto &held) {
								return held.Id == id;
							}
						))
						break;
				}
				const std::string nodeId = node.Id;
				engine::imagegraph::DynamicInput added{
					std::move(id), engine::imagegraph::ValueType::Image, std::nullopt
				};
				ApplyDocumentEdit(state, [&](Document &document) {
					if (SetImageGraphDynamicInput(document, nodeId, added, state.LastDiagnostic))
						state.LastDiagnostic = {};
				});
				ReloadCanvas(state);
			}
			ImGui::EndDisabled();
		}

		void DrawAnimationTrackControls(State &state, const Node &node, std::string_view property) {
			const size_t keyCount = static_cast<size_t>(std::count_if(
				state.Authored.Keyframes.begin(),
				state.Authored.Keyframes.end(),
				[&](const Keyframe &keyframe) {
					return keyframe.NodeId == node.Id && keyframe.Port == property;
				}
			));
			if (keyCount == 0) return;

			const auto track = std::find_if(
				state.Authored.Tracks.begin(), state.Authored.Tracks.end(), [&](const auto &candidate) {
					return candidate.NodeId == node.Id && candidate.Port == property;
				}
			);
			const bool hasTrack = track != state.Authored.Tracks.end();
			std::string end = hasTrack ? track->End : "hold";
			int64_t loopRange = hasTrack ? track->LoopRange : -1;
			const std::string section = "Animation (" + std::to_string(keyCount) + " keys)";
			if (!ImGui::TreeNode(section.c_str())) return;

			bool policyChanged = false;
			if (ImGui::BeginCombo("End", end.c_str())) {
				for (const char *choice : {"hold", "loop", "ping", "wrap"}) {
					const bool selected = end == choice;
					if (ImGui::Selectable(choice, selected)) {
						end = choice;
						policyChanged = true;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::InputScalar("Loop tail (-1 = all)", ImGuiDataType_S64, &loopRange)) {
				loopRange = std::clamp(loopRange, int64_t{-1}, static_cast<int64_t>(keyCount) - 1);
				policyChanged = true;
			}

			const std::string nodeId = node.Id;
			const std::string propertyId(property);
			const auto savePolicy = [&] {
				ApplyDocumentEdit(state, [&](Document &document) {
					if (SetImageGraphAnimationTrack(
							document, nodeId, propertyId, end, loopRange, state.LastDiagnostic
						))
						state.LastDiagnostic = {};
				});
			};
			if (hasTrack && policyChanged) savePolicy();
			if (!hasTrack) {
				ImGui::TextDisabled("No track override. Preview holds the final keyed value.");
				if (ImGui::SmallButton("Add track policy")) savePolicy();
			} else if (ImGui::SmallButton("Remove track policy")) {
				ApplyDocumentEdit(state, [&](Document &document) {
					if (RemoveImageGraphAnimationTrack(document, nodeId, propertyId, state.LastDiagnostic))
						state.LastDiagnostic = {};
				});
			}
			ImGui::TreePop();
		}

		void DrawInspector(State &state) {
			const std::string nodeId = SelectedNodeId(state);
			Node *node = FindNode(state.Authored, nodeId);
			if (node == nullptr) {
				ImGui::TextDisabled("Select one node to edit its authored values.");
				return;
			}
			ImGui::TextUnformatted(node->Type.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s", node->Id.c_str());
			const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
			if (schema == nullptr) {
				ImGui::TextDisabled("This node type is not registered in this build.");
				return;
			}
			if (schema->Properties.empty() && !schema->DynamicInputs) {
				ImGui::TextDisabled("No authored properties.");
				return;
			}
			ImGui::Separator();
			for (AuthoredValue &property : node->Values) {
				ImGui::PushID(node->Id.c_str());
				ImGui::PushID(property.Port.c_str());
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(property.Port.c_str());
				ImGui::SameLine(92.0f);
				ImGui::SetNextItemWidth(-1.0f);
				DrawValueWidget(state, node->Id, property);
				DrawAnimationTrackControls(state, *node, property.Port);
				ImGui::PopID();
				ImGui::PopID();
			}
			for (const engine::imagegraph::PropertySchema &property : schema->Properties) {
				if (FindValue(*node, property.Id) != nullptr) continue;
				const std::optional<Value> initial = ImageGraphPropertyDefault(node->Type, property.Id);
				const std::string propertyId(property.Id);
				ImGui::PushID(node->Id.c_str());
				ImGui::PushID(propertyId.c_str());
				ImGui::TextDisabled("%s: value missing", propertyId.c_str());
				ImGui::SameLine(92.0f);
				ImGui::BeginDisabled(!initial.has_value());
				if (ImGui::SmallButton("Initialize") && initial.has_value()) {
					const std::string nodeId = node->Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						if (Node *target = FindNode(document, nodeId);
							target != nullptr && FindValue(*target, propertyId) == nullptr) {
							target->Values.push_back({propertyId, *initial});
						}
					});
				}
				ImGui::EndDisabled();
				ImGui::PopID();
				ImGui::PopID();
			}
			DrawDynamicInputs(state, *node, *schema);
		}

		void DrawOutputs(State &state) {
			if (state.Authored.Outputs.empty()) {
				ImGui::TextDisabled("No outputs are bound.");
			} else {
				if (ImGui::BeginCombo("Output", state.SelectedOutput.c_str())) {
					for (const Output &output : state.Authored.Outputs) {
						const bool selected = output.Id == state.SelectedOutput;
						if (ImGui::Selectable(output.Id.c_str(), selected)) {
							state.SelectedOutput = output.Id;
							RequestPreview(state, true);
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Add output")) {
				const std::string nodeId = SelectedNodeId(state);
				const Node *node = FindNode(state.Authored, nodeId);
				const engine::imagegraph::NodeSchema *schema =
					node == nullptr ? nullptr : engine::imagegraph::FindSchema(node->Type);
				if (schema != nullptr) {
					const auto port =
						std::find_if(schema->Ports.begin(), schema->Ports.end(), [](const auto &entry) {
							return entry.Direction == engine::imagegraph::PortDirection::Output &&
								   (entry.Type == engine::imagegraph::ValueType::Image ||
									entry.Type == engine::imagegraph::ValueType::Scalar);
						});
					if (port != schema->Ports.end()) {
						std::string outputId;
						for (;;) {
							outputId = "output-" + std::to_string(state.NextOutputId++);
							if (std::none_of(
									state.Authored.Outputs.begin(),
									state.Authored.Outputs.end(),
									[&](const Output &output) { return output.Id == outputId; }
								)) {
								break;
							}
						}
						ApplyDocumentEdit(state, [&](Document &document) {
							document.Outputs.push_back({outputId, nodeId, std::string(port->Id)});
						});
						state.SelectedOutput = std::move(outputId);
					}
				}
			}
			const std::string bindingNodeId = SelectedNodeId(state);
			const Node *bindingNode = FindNode(state.Authored, bindingNodeId);
			const engine::imagegraph::NodeSchema *bindingSchema =
				bindingNode == nullptr ? nullptr : engine::imagegraph::FindSchema(bindingNode->Type);
			const engine::imagegraph::PortSchema *bindingPort = nullptr;
			if (bindingSchema != nullptr) {
				const auto found = std::find_if(
					bindingSchema->Ports.begin(), bindingSchema->Ports.end(), [](const auto &entry) {
						return entry.Direction == engine::imagegraph::PortDirection::Output &&
							   entry.Type == engine::imagegraph::ValueType::Image;
					}
				);
				if (found != bindingSchema->Ports.end()) bindingPort = &*found;
			}
			const bool canBind = !state.SelectedOutput.empty() && bindingPort != nullptr;
			ImGui::SameLine();
			ImGui::BeginDisabled(!canBind);
			if (ImGui::Button("Bind to selected node") && canBind) {
				const std::string port(bindingPort->Id);
				ApplyDocumentEdit(state, [&](Document &document) {
					if (!SetImageGraphOutput(
							document, state.SelectedOutput, bindingNodeId, port, state.LastDiagnostic
						))
						return;
					state.LastDiagnostic = {};
				});
			}
			ImGui::EndDisabled();
			if (!state.SelectedOutput.empty()) {
				ImGui::SameLine();
				if (ImGui::Button("Remove output")) {
					ApplyDocumentEdit(state, [&](Document &document) {
						document.Outputs.erase(
							std::remove_if(
								document.Outputs.begin(),
								document.Outputs.end(),
								[&](const Output &output) { return output.Id == state.SelectedOutput; }
							),
							document.Outputs.end()
						);
					});
					state.SelectedOutput =
						state.Authored.Outputs.empty() ? std::string{} : state.Authored.Outputs.front().Id;
					RequestPreview(state, true);
				}
			}
			for (const Output &output : state.Authored.Outputs) {
				ImGui::TextDisabled(
					"%s  <-  %s.%s", output.Id.c_str(), output.NodeId.c_str(), output.Port.c_str()
				);
			}
		}

		std::string NewDocumentId(const Document &document, std::string_view prefix) {
			for (uint64_t index = 1;; index++) {
				std::string id(prefix);
				id += std::to_string(index);
				const bool used =
					std::any_of(
						document.Nodes.begin(),
						document.Nodes.end(),
						[&](const Node &node) { return node.Id == id; }
					) ||
					std::any_of(
						document.Groups.begin(),
						document.Groups.end(),
						[&](const auto &group) { return group.Id == id; }
					) ||
					std::any_of(
						document.Junctions.begin(),
						document.Junctions.end(),
						[&](const auto &junction) { return junction.Id == id; }
					) ||
					std::any_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
						return std::any_of(group.Ports.begin(), group.Ports.end(), [&](const auto &port) {
							return port.Id == id;
						});
					});
				if (!used) return id;
			}
		}

		using EndpointChoice = std::pair<std::string, std::string>;

		std::vector<EndpointChoice> EndpointChoices(const Document &document) {
			std::vector<EndpointChoice> choices;
			choices.reserve(document.Nodes.size() + document.Junctions.size());
			for (const Node &node : document.Nodes)
				choices.emplace_back("node:" + node.Id, node.Type + "  " + node.Id);
			for (const auto &junction : document.Junctions)
				choices.emplace_back("junction:" + junction.Id, "Junction  " + junction.Id);
			return choices;
		}

		bool DrawEndpointCombo(
			std::string_view label, const std::vector<EndpointChoice> &choices, std::string &selected
		) {
			std::string preview = "Select endpoint";
			for (const auto &[id, title] : choices)
				if (id == selected) preview = title;
			bool changed = false;
			if (ImGui::BeginCombo(std::string(label).c_str(), preview.c_str())) {
				for (const auto &[id, title] : choices) {
					const bool active = selected == id;
					if (ImGui::Selectable(title.c_str(), active)) {
						selected = id;
						changed = true;
					}
					if (active) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			return changed;
		}

		std::vector<std::string>
		EndpointPorts(const Document &document, std::string_view endpoint, bool output) {
			std::vector<std::string> ports;
			constexpr std::string_view nodePrefix = "node:";
			constexpr std::string_view junctionPrefix = "junction:";
			if (endpoint.starts_with(nodePrefix)) {
				const Node *node = FindNode(document, endpoint.substr(nodePrefix.size()));
				const engine::imagegraph::NodeSchema *schema =
					node == nullptr ? nullptr : engine::imagegraph::FindSchema(node->Type);
				if (schema != nullptr) {
					const auto direction = output ? engine::imagegraph::PortDirection::Output
												  : engine::imagegraph::PortDirection::Input;
					for (const auto &port : schema->Ports)
						if (port.Direction == direction) ports.emplace_back(port.Id);
					if (!output)
						for (const auto &port : node->DynamicInputs)
							ports.push_back(port.Id);
				}
			} else if (endpoint.starts_with(junctionPrefix)) {
				const std::string_view id = endpoint.substr(junctionPrefix.size());
				if (std::any_of(
						document.Junctions.begin(), document.Junctions.end(), [&](const auto &junction) {
							return junction.Id == id;
						}
					))
					ports.emplace_back("value");
			}
			return ports;
		}

		bool
		DrawPortCombo(std::string_view label, const std::vector<std::string> &ports, std::string &selected) {
			std::string preview = "Select port";
			if (std::find(ports.begin(), ports.end(), selected) != ports.end())
				preview = selected;
			else if (!ports.empty()) {
				selected = ports.front();
				preview = selected;
			}
			bool changed = false;
			if (ImGui::BeginCombo(std::string(label).c_str(), preview.c_str())) {
				for (const std::string &port : ports) {
					const bool active = selected == port;
					if (ImGui::Selectable(port.c_str(), active)) {
						selected = port;
						changed = true;
					}
					if (active) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			return changed;
		}

		void DrawGroups(State &state) {
			const auto selectedGroup = std::find_if(
				state.Authored.Groups.begin(), state.Authored.Groups.end(), [&](const auto &group) {
					return group.Id == state.SelectedGroup;
				}
			);
			std::string groupPreview = "Select group";
			if (selectedGroup != state.Authored.Groups.end())
				groupPreview = selectedGroup->Name + "  " + selectedGroup->Id;
			if (ImGui::BeginCombo("Group", groupPreview.c_str())) {
				for (const auto &group : state.Authored.Groups) {
					const bool active = group.Id == state.SelectedGroup;
					const std::string label = group.Name + "  " + group.Id;
					if (ImGui::Selectable(label.c_str(), active)) {
						state.SelectedGroup = group.Id;
						std::fill(state.GroupName.begin(), state.GroupName.end(), '\0');
						std::copy_n(
							group.Name.c_str(),
							std::min(group.Name.size(), state.GroupName.size() - 1),
							state.GroupName.data()
						);
					}
					if (active) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::Button("Add root group") &&
				state.Authored.Groups.size() < engine::imagegraph::Limits::MaximumGroups) {
				const std::string id = NewDocumentId(state.Authored, "group-");
				const std::string name = "Group " + id.substr(id.find_last_of('-') + 1);
				ApplyDocumentEdit(state, [&](Document &document) {
					if (AddImageGraphGroup(document, id, name, {}, state.LastDiagnostic))
						state.LastDiagnostic = {};
				});
				state.SelectedGroup = id;
				std::fill(state.GroupName.begin(), state.GroupName.end(), '\0');
				std::copy_n(
					name.c_str(), std::min(name.size(), state.GroupName.size() - 1), state.GroupName.data()
				);
				ReloadCanvas(state);
			}
			ImGui::SameLine();
			const auto parentGroup = std::find_if(
				state.Authored.Groups.begin(), state.Authored.Groups.end(), [&](const auto &group) {
					return group.Id == state.SelectedGroup;
				}
			);
			ImGui::BeginDisabled(parentGroup == state.Authored.Groups.end());
			if (ImGui::Button("Add child group") && parentGroup != state.Authored.Groups.end()) {
				const std::string parent = parentGroup->Id;
				const std::string id = NewDocumentId(state.Authored, "group-");
				const std::string name = "Group " + id.substr(id.find_last_of('-') + 1);
				ApplyDocumentEdit(state, [&](Document &document) {
					if (AddImageGraphGroup(document, id, name, parent, state.LastDiagnostic))
						state.LastDiagnostic = {};
				});
				state.SelectedGroup = id;
				std::fill(state.GroupName.begin(), state.GroupName.end(), '\0');
				std::copy_n(
					name.c_str(), std::min(name.size(), state.GroupName.size() - 1), state.GroupName.data()
				);
				ReloadCanvas(state);
			}
			ImGui::EndDisabled();

			const auto groupNow = std::find_if(
				state.Authored.Groups.begin(), state.Authored.Groups.end(), [&](const auto &group) {
					return group.Id == state.SelectedGroup;
				}
			);
			if (groupNow != state.Authored.Groups.end()) {
				ImGui::Separator();
				ImGui::TextUnformatted("Group name");
				ImGui::SameLine(92.0f);
				ImGui::InputText("##group-name", state.GroupName.data(), state.GroupName.size());
				ImGui::SameLine();
				if (ImGui::SmallButton("Apply name") && state.GroupName[0] != '\0') {
					const std::string id = groupNow->Id;
					const std::string name(state.GroupName.data());
					ApplyDocumentEdit(state, [&](Document &document) {
						auto found = std::find_if(
							document.Groups.begin(), document.Groups.end(), [&](const auto &item) {
								return item.Id == id;
							}
						);
						if (found != document.Groups.end()) found->Name = name;
					});
					ReloadCanvas(state);
				}
				const std::string selectedNode = SelectedNodeId(state);
				ImGui::SameLine();
				ImGui::BeginDisabled(selectedNode.empty());
				if (ImGui::SmallButton("Move selected node here") && !selectedNode.empty()) {
					const std::string groupId = groupNow->Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						if (Node *node = FindNode(document, selectedNode)) node->GroupId = groupId;
					});
					ReloadCanvas(state);
				}
				ImGui::EndDisabled();

				ImGui::Separator();
				ImGui::TextUnformatted("Interface socket");
				if (ImGui::BeginCombo(
						"Direction",
						state.GroupPortDirection == engine::imagegraph::PortDirection::Input ? "Input"
																							 : "Output"
					)) {
					for (const auto direction :
						 {engine::imagegraph::PortDirection::Input,
						  engine::imagegraph::PortDirection::Output}) {
						const char *label =
							direction == engine::imagegraph::PortDirection::Input ? "Input" : "Output";
						if (ImGui::Selectable(label, state.GroupPortDirection == direction))
							state.GroupPortDirection = direction;
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				if (ImGui::BeginCombo("Type", ValueTypeName(state.GroupPortType))) {
					for (const auto type :
						 {engine::imagegraph::ValueType::Boolean,
						  engine::imagegraph::ValueType::Integer,
						  engine::imagegraph::ValueType::Scalar,
						  engine::imagegraph::ValueType::Text,
						  engine::imagegraph::ValueType::Colour,
						  engine::imagegraph::ValueType::Vector2,
						  engine::imagegraph::ValueType::Image,
						  engine::imagegraph::ValueType::Array,
						  engine::imagegraph::ValueType::Gradient,
						  engine::imagegraph::ValueType::Area,
						  engine::imagegraph::ValueType::Curve,
						  engine::imagegraph::ValueType::Vector4,
						  engine::imagegraph::ValueType::Path2D}) {
						const bool active = state.GroupPortType == type;
						if (ImGui::Selectable(ValueTypeName(type), active)) state.GroupPortType = type;
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(groupNow->Ports.size() >= engine::imagegraph::Limits::MaximumGroupPorts);
				if (ImGui::SmallButton("Add socket") &&
					groupNow->Ports.size() < engine::imagegraph::Limits::MaximumGroupPorts) {
					const std::string groupId = groupNow->Id;
					const std::string portId = NewDocumentId(state.Authored, "port-");
					const std::string junctionId = NewDocumentId(state.Authored, "junction-");
					engine::imagegraph::GroupPort port{portId, junctionId, state.GroupPortDirection};
					engine::imagegraph::Junction junction{
						junctionId, groupId, state.GroupPortType, std::nullopt
					};
					ApplyDocumentEdit(state, [&](Document &document) {
						if (AddImageGraphGroupPort(document, groupId, port, junction, state.LastDiagnostic))
							state.LastDiagnostic = {};
					});
					ReloadCanvas(state);
				}
				ImGui::EndDisabled();
				for (const auto &port : groupNow->Ports) {
					ImGui::PushID(port.Id.c_str());
					const auto junction = std::find_if(
						state.Authored.Junctions.begin(),
						state.Authored.Junctions.end(),
						[&](const auto &item) { return item.Id == port.JunctionId; }
					);
					ImGui::Text(
						"%s %s %s",
						port.Id.c_str(),
						port.Direction == engine::imagegraph::PortDirection::Input ? "in" : "out",
						junction == state.Authored.Junctions.end() ? "unknown" : ValueTypeName(junction->Type)
					);
					ImGui::SameLine();
					if (ImGui::SmallButton("Remove socket")) {
						const std::string groupId = groupNow->Id;
						const std::string junctionId = port.JunctionId;
						ApplyDocumentEdit(state, [&](Document &document) {
							auto group = std::find_if(
								document.Groups.begin(), document.Groups.end(), [&](const auto &item) {
									return item.Id == groupId;
								}
							);
							if (group != document.Groups.end())
								std::erase_if(group->Ports, [&](const auto &item) {
									return item.JunctionId == junctionId;
								});
							std::erase_if(document.Junctions, [&](const auto &item) {
								return item.Id == junctionId;
							});
							std::erase_if(document.Links, [&](const auto &item) {
								return item.FromNode == junctionId || item.ToNode == junctionId;
							});
						});
						ReloadCanvas(state);
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
			}

			ImGui::Separator();
			ImGui::TextUnformatted("Route");
			const std::vector<EndpointChoice> endpoints = EndpointChoices(state.Authored);
			DrawEndpointCombo("From##route-from-endpoint", endpoints, state.RouteFromEndpoint);
			const std::vector<std::string> sourcePorts =
				EndpointPorts(state.Authored, state.RouteFromEndpoint, true);
			ImGui::SameLine();
			DrawPortCombo("##from-port", sourcePorts, state.RouteFromPort);
			DrawEndpointCombo("To##route-to-endpoint", endpoints, state.RouteToEndpoint);
			const std::vector<std::string> destinationPorts =
				EndpointPorts(state.Authored, state.RouteToEndpoint, false);
			ImGui::SameLine();
			DrawPortCombo("##to-port", destinationPorts, state.RouteToPort);
			ImGui::BeginDisabled(sourcePorts.empty() || destinationPorts.empty());
			if (ImGui::SmallButton("Add route") && !sourcePorts.empty() && !destinationPorts.empty()) {
				const std::string fromId =
					state.RouteFromEndpoint.substr(state.RouteFromEndpoint.find(':') + 1);
				const std::string toId = state.RouteToEndpoint.substr(state.RouteToEndpoint.find(':') + 1);
				ApplyDocumentEdit(state, [&](Document &document) {
					if (AddImageGraphRoute(
							document,
							{fromId, state.RouteFromPort, toId, state.RouteToPort},
							state.LastDiagnostic
						))
						state.LastDiagnostic = {};
				});
				ReloadCanvas(state);
			}
			ImGui::EndDisabled();
			if (state.LastDiagnostic.Code != Status::Ok)
				ImGui::TextWrapped("Route: %s", state.LastDiagnostic.Message.c_str());
			for (size_t index = 0; index < state.Authored.Links.size(); index++) {
				const auto &link = state.Authored.Links[index];
				ImGui::PushID(static_cast<int>(index));
				ImGui::Text(
					"%s.%s -> %s.%s",
					link.FromNode.c_str(),
					link.FromPort.c_str(),
					link.ToNode.c_str(),
					link.ToPort.c_str()
				);
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove route")) {
					ApplyDocumentEdit(state, [&](Document &document) {
						if (RemoveImageGraphRoute(document, index, state.LastDiagnostic))
							state.LastDiagnostic = {};
					});
					ReloadCanvas(state);
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
		}

		void AddKeyframe(State &state, const std::string &nodeId, const std::string &port) {
			ApplyDocumentEdit(state, [&](Document &document) {
				if (!SetImageGraphKeyframe(
						document, nodeId, port, state.Playback.CurrentTick, "step", state.LastDiagnostic
					))
					return;
				state.LastDiagnostic = {};
			});
		}

		void DrawKeyframeEaseSide(State &state, size_t index, const Keyframe &frame, bool incoming) {
			if (frame.Interpolation != "source" || !frame.Ease) {
				ImGui::TextUnformatted("-");
				return;
			}

			auto ease = *frame.Ease;
			std::string &sideType = incoming ? ease.InType : ease.OutType;
			engine::imagegraph::Vector2 &handle = incoming ? ease.In : ease.Out;
			bool changed = false;
			if (ImGui::BeginCombo("##ease-side", sideType.c_str())) {
				for (const char *choice : {"linear", "bezier", "cut"}) {
					const bool selected = sideType == choice;
					if (ImGui::Selectable(choice, selected)) {
						sideType = choice;
						changed = true;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(54.0f);
			changed |= ImGui::InputDouble("##ease-x", &handle.X, 0.05, 0.25, "%.2f");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(54.0f);
			changed |= ImGui::InputDouble("##ease-y", &handle.Y, 0.05, 0.25, "%.2f");
			if (!changed) return;

			ApplyDocumentEdit(state, [&](Document &document) {
				if (SetImageGraphKeyframeEase(document, index, ease, state.LastDiagnostic))
					state.LastDiagnostic = {};
			});
		}

		void DrawKeyframeSineDriver(State &state, size_t index, const Keyframe &frame) {
			if (!std::holds_alternative<double>(frame.Data)) {
				ImGui::TextUnformatted("Scalar only");
				return;
			}
			if (ImGui::SmallButton(frame.SineDriver ? "Sine..." : "Add sine")) {
				if (!frame.SineDriver) {
					ApplyDocumentEdit(state, [&](Document &document) {
						if (SetImageGraphKeyframeSineDriver(
								document,
								index,
								engine::imagegraph::KeyframeSineDriver{},
								state.LastDiagnostic
							))
							state.LastDiagnostic = {};
					});
				}
				ImGui::OpenPopup("##sine-driver");
			}
			if (!ImGui::BeginPopup("##sine-driver")) return;

			auto driver = frame.SineDriver.value_or(engine::imagegraph::KeyframeSineDriver{});
			bool changed = false;
			ImGui::SetNextItemWidth(120.0f);
			changed |= ImGui::InputDouble("Frequency", &driver.Frequency, 0.1, 1.0, "%.6g");
			ImGui::SetNextItemWidth(120.0f);
			changed |= ImGui::InputDouble("Amplitude", &driver.Amplitude, 0.1, 1.0, "%.6g");
			ImGui::SetNextItemWidth(120.0f);
			changed |= ImGui::InputDouble("Phase", &driver.Phase, 0.01, 0.1, "%.6g");
			ImGui::SetNextItemWidth(120.0f);
			changed |= ImGui::InputDouble("Smooth", &driver.Smooth, 0.05, 0.1, "%.4f");
			if (changed) {
				ApplyDocumentEdit(state, [&](Document &document) {
					if (SetImageGraphKeyframeSineDriver(document, index, driver, state.LastDiagnostic))
						state.LastDiagnostic = {};
				});
			}
			if (ImGui::SmallButton("Remove sine")) {
				ApplyDocumentEdit(state, [&](Document &document) {
					if (SetImageGraphKeyframeSineDriver(document, index, std::nullopt, state.LastDiagnostic))
						state.LastDiagnostic = {};
				});
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		void DrawTimeline(State &state) {
			if (ImGui::Button(state.Playback.Playing ? "Pause" : "Play")) {
				if (!state.Playback.Playing && (state.Playback.CurrentTick < state.Playback.StartTick ||
												state.Playback.CurrentTick > state.Playback.EndTick)) {
					if (SetImageGraphPlaybackFrame(
							state.Playback, static_cast<double>(state.Playback.StartTick)
						))
						RequestPreview(state);
				}
				if (!state.Playback.Playing) state.Playback.Direction = 1;
				if (state.Playback.CurrentTick >= state.Playback.EndTick && !state.Playback.Loop &&
					!state.Playback.PingPong &&
					SetImageGraphPlaybackFrame(state.Playback, static_cast<double>(state.Playback.StartTick)))
					RequestPreview(state);
				state.Playback.Playing = !state.Playback.Playing;
				state.Playback.Accumulator = 0.0;
			}
			ImGui::SameLine();
			if (ImGui::Button("Step -1") &&
				(state.Playback.CurrentTick > state.Playback.StartTick || state.Playback.Subframe > 0.0)) {
				(void)SetImageGraphPlaybackFrame(
					state.Playback,
					static_cast<double>(state.Playback.CurrentTick) + state.Playback.Subframe - 1.0
				);
				state.Playback.Direction = -1;
				RequestPreview(state);
			}
			ImGui::SameLine();
			if (ImGui::Button("Step +1") && state.Playback.CurrentTick < state.Playback.EndTick) {
				(void)SetImageGraphPlaybackFrame(
					state.Playback,
					static_cast<double>(state.Playback.CurrentTick) + state.Playback.Subframe + 1.0
				);
				state.Playback.Direction = 1;
				RequestPreview(state);
			}
			ImGui::SameLine();
			const char *playbackMode = state.Playback.PingPong ? "pingpong"
									   : state.Playback.Loop   ? "loop"
															   : "stop";
			bool modeChanged = false;
			if (ImGui::BeginCombo("Playback", playbackMode)) {
				for (const char *choice : {"loop", "stop", "pingpong"}) {
					const bool selected = std::string_view(playbackMode) == choice;
					if (ImGui::Selectable(choice, selected)) {
						state.Playback.Loop = std::string_view(choice) == "loop";
						state.Playback.PingPong = std::string_view(choice) == "pingpong";
						state.Playback.Direction = 1;
						modeChanged = true;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(72.0f);
			const bool fpsChanged =
				ImGui::InputDouble("FPS", &state.Playback.FramesPerSecond, 1.0, 5.0, "%.6g");
			if (fpsChanged) {
				const double frameDuration = 1.0 / state.Playback.FramesPerSecond;
				if (!std::isfinite(state.Playback.FramesPerSecond) || state.Playback.FramesPerSecond <= 0.0 ||
					!std::isfinite(frameDuration) || frameDuration <= 0.0) {
					state.Playback.FramesPerSecond = 30.0;
				}
			}
			ImGui::SameLine();
			ImGui::TextUnformatted("Frames");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(86.0f);
			bool rangeChanged =
				ImGui::InputScalar("##timeline-frames", ImGuiDataType_U64, &state.Playback.TotalFrames);
			const bool framesChanged = rangeChanged;
			ImGui::SameLine();
			ImGui::TextUnformatted("Range");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(92.0f);
			const bool startChanged =
				ImGui::InputScalar("##range-start", ImGuiDataType_U64, &state.Playback.StartTick);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(92.0f);
			const bool endChanged =
				ImGui::InputScalar("##range-end", ImGuiDataType_U64, &state.Playback.EndTick);
			rangeChanged = rangeChanged || startChanged || endChanged;
			const double previousFrame =
				static_cast<double>(state.Playback.CurrentTick) + state.Playback.Subframe;
			const uint64_t maximumFrames = engine::imagegraph::Limits::MaximumTick + 1;
			if (framesChanged) {
				state.Playback.TotalFrames =
					std::clamp(state.Playback.TotalFrames, uint64_t{1}, maximumFrames);
				state.Playback.StartTick = std::min(state.Playback.StartTick, state.Playback.TotalFrames - 1);
				state.Playback.EndTick = std::clamp(
					state.Playback.EndTick, state.Playback.StartTick, state.Playback.TotalFrames - 1
				);
			} else if (startChanged) {
				state.Playback.StartTick = std::min(state.Playback.StartTick, maximumFrames - 1);
				state.Playback.TotalFrames =
					std::max(state.Playback.TotalFrames, state.Playback.StartTick + 1);
				state.Playback.EndTick = std::max(state.Playback.EndTick, state.Playback.StartTick);
			} else if (endChanged) {
				state.Playback.EndTick = std::min(state.Playback.EndTick, maximumFrames - 1);
				state.Playback.TotalFrames = std::max(state.Playback.TotalFrames, state.Playback.EndTick + 1);
				state.Playback.StartTick = std::min(state.Playback.StartTick, state.Playback.EndTick);
			}
			state.Playback.StartTick = std::min(state.Playback.StartTick, state.Playback.TotalFrames - 1);
			state.Playback.EndTick =
				std::clamp(state.Playback.EndTick, state.Playback.StartTick, state.Playback.TotalFrames - 1);
			const bool cursorChanged = SetImageGraphPlaybackFrame(state.Playback, previousFrame);
			if (rangeChanged || modeChanged || fpsChanged) {
				if (cursorChanged) RequestPreview(state);
				SavePlaybackIfAuthored(state);
			}
			ImGui::SameLine();
			if (state.Authored.Timeline) {
				if (ImGui::SmallButton("Remove saved range")) (void)RemoveSavedTimeline(state);
			} else if (ImGui::SmallButton("Save range")) {
				(void)SavePlaybackTimeline(state);
			}
			ImGui::TextDisabled(
				"%s at %.6g fps. %s",
				state.Playback.Playing ? "Playing" : "Paused",
				state.Playback.FramesPerSecond,
				state.Authored.Timeline ? "Timeline saved."
										: "Use Save range to store FPS and playback settings."
			);
			ImGui::TextUnformatted("Frame");
			ImGui::SameLine(96.0f);
			ImGui::SetNextItemWidth(140.0f);
			double frame = static_cast<double>(state.Playback.CurrentTick) + state.Playback.Subframe;
			if (ImGui::InputDouble("##timeline-frame", &frame, 0.01, 1.0, "%.6f")) {
				if (SetImageGraphPlaybackFrame(state.Playback, frame)) RequestPreview(state);
			}
			ImGui::SameLine();
			if (ImGui::Button("Previous key")) {
				const uint64_t key =
					PreviousImageGraphKey(state.Authored, state.Playback.CurrentTick).value_or(0);
				if (SetImageGraphPlaybackFrame(state.Playback, static_cast<double>(key)))
					RequestPreview(state);
			}
			ImGui::SameLine();
			if (ImGui::Button("Next key")) {
				if (const auto next = NextImageGraphKey(state.Authored, state.Playback.CurrentTick);
					next.has_value() &&
					SetImageGraphPlaybackFrame(state.Playback, static_cast<double>(*next)))
					RequestPreview(state);
			}
			const std::string selectedNode = SelectedNodeId(state);
			Node *node = FindNode(state.Authored, selectedNode);
			if (node != nullptr) {
				for (AuthoredValue &value : node->Values) {
					ImGui::PushID(value.Port.c_str());
					ImGui::TextUnformatted(value.Port.c_str());
					ImGui::SameLine(96.0f);
					if (ImGui::Button("Set key")) AddKeyframe(state, node->Id, value.Port);
					ImGui::SameLine();
					if (ImGui::Button("Remove at tick")) {
						ApplyDocumentEdit(state, [&](Document &document) {
							if (!RemoveImageGraphKeyframe(
									document,
									node->Id,
									value.Port,
									state.Playback.CurrentTick,
									state.LastDiagnostic
								))
								return;
							state.LastDiagnostic = {};
						});
					}
					ImGui::PopID();
				}
			}
			ImGui::Separator();
			const bool hasLegacyCubic = std::any_of(
				state.Authored.Keyframes.begin(), state.Authored.Keyframes.end(), [](const Keyframe &frame) {
					return frame.Interpolation == "cubic";
				}
			);
			if (hasLegacyCubic) {
				ImGui::TextDisabled(
					"Legacy cubic keys are retained and report UnsupportedExecution. Choose source for "
					"editable handles."
				);
			} else {
				ImGui::TextDisabled(
					"Source easing supports linear, Bezier and cut sides with incoming and outgoing handles."
				);
			}
			if (ImGui::BeginTable("##keyframes", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn("Property");
				ImGui::TableSetupColumn("Tick");
				ImGui::TableSetupColumn("Interpolation");
				ImGui::TableSetupColumn("Ease in");
				ImGui::TableSetupColumn("Ease out");
				ImGui::TableSetupColumn("Driver");
				ImGui::TableSetupColumn("Action");
				ImGui::TableHeadersRow();
				for (size_t index = 0; index < state.Authored.Keyframes.size(); index++) {
					const Keyframe frame = state.Authored.Keyframes[index];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					const std::string label = frame.NodeId + "." + frame.Port;
					if (ImGui::Selectable(
							label.c_str(),
							state.Playback.CurrentTick == frame.Tick && state.Playback.Subframe == 0.0,
							ImGuiSelectableFlags_SpanAllColumns
						)) {
						if (SetImageGraphPlaybackFrame(state.Playback, static_cast<double>(frame.Tick)))
							RequestPreview(state);
						if (const auto found = state.Ids.ToCanvas.find(frame.NodeId);
							found != state.Ids.ToCanvas.end()) {
							state.Canvas.Select(found->second);
							state.Canvas.Centre(state.Graph, found->second);
						}
					}
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%llu", static_cast<unsigned long long>(frame.Tick));
					ImGui::TableSetColumnIndex(2);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::BeginCombo("##interpolation", frame.Interpolation.c_str())) {
						for (const char *choice : {"step", "linear", "cubic", "source"}) {
							const bool selected = frame.Interpolation == choice;
							if (ImGui::Selectable(choice, selected)) {
								ApplyDocumentEdit(state, [&](Document &document) {
									if (SetImageGraphKeyframeInterpolation(
											document, index, choice, state.LastDiagnostic
										))
										state.LastDiagnostic = {};
								});
							}
							if (selected) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					ImGui::PopID();
					ImGui::TableSetColumnIndex(3);
					ImGui::PushID(static_cast<int>(index));
					ImGui::PushID("ease-in");
					DrawKeyframeEaseSide(state, index, frame, true);
					ImGui::PopID();
					ImGui::PopID();
					ImGui::TableSetColumnIndex(4);
					ImGui::PushID(static_cast<int>(index));
					ImGui::PushID("ease-out");
					DrawKeyframeEaseSide(state, index, frame, false);
					ImGui::PopID();
					ImGui::PopID();
					ImGui::TableSetColumnIndex(5);
					ImGui::PushID(static_cast<int>(index));
					DrawKeyframeSineDriver(state, index, frame);
					ImGui::PopID();
					ImGui::TableSetColumnIndex(6);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::SmallButton("Delete")) {
						ApplyDocumentEdit(state, [&](Document &document) {
							if (index < document.Keyframes.size()) {
								document.Keyframes.erase(
									document.Keyframes.begin() + static_cast<std::ptrdiff_t>(index)
								);
							}
						});
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}

		void NavigateDiagnostic(State &state) {
			if (state.LastDiagnostic.NodeId.empty()) return;
			ImGui::SameLine();
			if (ImGui::SmallButton("Go to node")) {
				const auto found = state.Ids.ToCanvas.find(state.LastDiagnostic.NodeId);
				if (found != state.Ids.ToCanvas.end()) {
					state.Canvas.Select(found->second);
					state.Canvas.Centre(state.Graph, found->second);
				}
			}
		}

		void DrawAssetsAndSinks(State &state, engine::render::Renderer &renderer) {
			ImGui::TextUnformatted("Image assets");
			ImGui::TextDisabled("No external image inputs are declared by the available node schemas.");
			ImGui::Separator();
			ImGui::TextUnformatted("Recorded mono audio");
			ImGui::InputTextWithHint(
				"##image-audio-capture-path",
				"Path to audio-capture 1 document",
				state.AudioCapturePath,
				sizeof(state.AudioCapturePath)
			);
			if (ImGui::Button("Load audio capture")) OpenAudioCapture(state);
			ImGui::SameLine();
			if (ImGui::Button("Clear audio capture")) {
				state.AudioFrames.clear();
				state.AudioCapturePathDisplay.clear();
				state.AudioCaptureMessage = "no recorded audio loaded";
				RequestPreview(state, true);
			}
			if (!state.AudioCapturePathDisplay.empty())
				ImGui::TextWrapped("Capture: %s", state.AudioCapturePathDisplay.c_str());
			if (!state.AudioCaptureMessage.empty())
				ImGui::TextWrapped("Audio input: %s", state.AudioCaptureMessage.c_str());
			ImGui::TextDisabled(
				"Audio source selects one exact source ID and tick. Live devices are not read by the "
				"evaluator."
			);
			ImGui::Separator();
			ImGui::TextUnformatted("PXCX archive");
			ImGui::InputTextWithHint(
				"##image-pxcx-path", "Path to .pxcx archive", state.PxcxPath, sizeof(state.PxcxPath)
			);
			if (ImGui::Button("Open PXCX")) OpenPxcx(state);
			if (!state.PxcxOpenError.empty()) ImGui::TextWrapped("PXCX: %s", state.PxcxOpenError.c_str());
			if (state.ImportedPxcx.has_value()) {
				const engine::bake::PxcxArchive &archive = *state.ImportedPxcx;
				ImGui::TextWrapped("Retained source: %s", state.PxcxPathDisplay.c_str());
				ImGui::Text(
					"Archive %zu bytes  graph %zu bytes  %zu nodes  %zu links",
					archive.OriginalBytes.size(),
					archive.GraphJson.size(),
					archive.Nodes.size(),
					archive.Links.size()
				);
				if (state.PxcxReferenceThumbnail.Pixels.empty()) {
					ImGui::TextDisabled("This archive has no decoded embedded thumbnail.");
				} else {
					ImGui::TextUnformatted(
						"Embedded PXCX source thumbnail, reference only, not a graph render."
					);
					if (UploadPxcxReferenceThumbnail(state, renderer)) {
						void *handle = renderer.TextureHandle(state.CurrentPxcxThumbnailTexture);
						if (handle != nullptr) {
							ImGui::Image(reinterpret_cast<ImTextureID>(handle), ImVec2(256.0f, 256.0f));
						} else {
							ImGui::TextDisabled("PXCX source thumbnail texture is unavailable.");
						}
					} else if (!state.PxcxThumbnailMessage.empty()) {
						ImGui::TextWrapped("PXCX thumbnail: %s", state.PxcxThumbnailMessage.c_str());
					}
				}
				const size_t nativeCount = static_cast<size_t>(std::count_if(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [](const Node &node) {
						return engine::imagegraph::FindSchema(node.Type) != nullptr;
					}
				));
				ImGui::Text(
					"Native mappings: %zu  opaque nodes: %zu. Opaque nodes are preserved and cannot execute.",
					nativeCount,
					state.Authored.Nodes.size() - nativeCount
				);
				for (const Diagnostic &diagnostic : state.PxcxDiagnostics) {
					ImGui::PushID(diagnostic.NodeId.c_str());
					ImGui::TextWrapped("%s: %s", diagnostic.NodeId.c_str(), diagnostic.Message.c_str());
					const auto found = state.Ids.ToCanvas.find(diagnostic.NodeId);
					if (found != state.Ids.ToCanvas.end() && ImGui::SmallButton("Go to node")) {
						state.Canvas.Select(found->second);
						state.Canvas.Centre(state.Graph, found->second);
					}
					ImGui::PopID();
				}
			}
			ImGui::Separator();
			ImGui::TextUnformatted("Native graph document");
			ImGui::InputTextWithHint(
				"##image-graph-name", "Graph name", state.GraphName, sizeof(state.GraphName)
			);
			if (ImGui::Button("Open .graph")) OpenNativeGraph(state);
			ImGui::SameLine();
			if (ImGui::Button("Save .graph")) SaveNativeGraph(state);
			if (!state.GraphIoMessage.empty()) ImGui::TextWrapped("Graph: %s", state.GraphIoMessage.c_str());
			ImGui::TextDisabled("Files use Assets/imagegraphs/<name>.graph. PXCX bytes remain unchanged.");
			ImGui::Separator();
			ImGui::TextUnformatted("Output sinks");
			const std::vector<ImageComposerSinkRow> rows =
				DescribeImageComposerSinks(state.Authored, state.SelectedOutput);
			if (rows.empty()) {
				ImGui::TextDisabled("No image outputs are bound.");
			} else {
				for (const ImageComposerSinkRow &row : rows) {
					ImGui::PushID(row.OutputId.c_str());
					if (ImGui::Selectable(row.OutputId.c_str(), row.Selected)) {
						state.SelectedOutput = row.OutputId;
						RequestPreview(state, true);
					}
					ImGui::SameLine();
					ImGui::TextDisabled("<- %s.%s", row.NodeId.c_str(), row.Port.c_str());
					if (!row.TargetExists)
						ImGui::TextWrapped("Output source %s is missing.", row.NodeId.c_str());
					ImGui::PopID();
				}
			}
			ImGui::Separator();
			ImGui::TextDisabled(
				"Preview uses the selected output. Scene texture bindings are set on the selected entity."
			);
		}

		void DrawDiagnostics(State &state) {
			if (!state.AdapterError.empty()) ImGui::TextWrapped("Canvas: %s", state.AdapterError.c_str());
			if (state.LastDiagnostic.Code == Status::Ok) {
				ImGui::TextUnformatted("No current compile or evaluation error.");
			} else {
				ImGui::TextWrapped("%s", state.LastDiagnostic.Message.c_str());
				if (!state.LastDiagnostic.NodeId.empty()) {
					ImGui::Text("Node: %s", state.LastDiagnostic.NodeId.c_str());
				}
				if (!state.LastDiagnostic.Port.empty())
					ImGui::Text("Port: %s", state.LastDiagnostic.Port.c_str());
				NavigateDiagnostic(state);
			}
			ImGui::Separator();
			ImGui::Text(
				"Document: %zu nodes, %zu links, %zu keyframes",
				state.Authored.Nodes.size(),
				state.Authored.Links.size(),
				state.Authored.Keyframes.size()
			);
			ImGui::Text(
				"Preview budget: %u by %u, %zu bytes",
				PREVIEW_MAXIMUM_DIMENSION,
				PREVIEW_MAXIMUM_DIMENSION,
				PREVIEW_MAXIMUM_BYTES
			);
			ImGui::Text(
				"Evaluation: deterministic CPU, preview cache %zu / %zu bytes",
				state.PreviewCache.HeldBytes(),
				IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES * PREVIEW_MAXIMUM_BYTES
			);
			ImGui::TextUnformatted("Profiling: image composer preview appears in the F5 frame graph.");
			if (!state.SinkMessage.empty()) ImGui::TextWrapped("Texture sink: %s", state.SinkMessage.c_str());
			ImGui::Checkbox("Show diagnostic details", &state.ShowAdvancedDiagnostics);
			if (state.ShowAdvancedDiagnostics && state.LastDiagnostic.Code != Status::Ok) {
				ImGui::Text("Status code: %u", static_cast<unsigned>(state.LastDiagnostic.Code));
			}
		}

		void DrawPreview(State &state, engine::render::Renderer &renderer) {
			if (state.HaveScalarPreview) {
				ImGui::Text(
					"Scalar output %s  tick %llu  value %.17g",
					state.ScalarPreviewPort.c_str(),
					static_cast<unsigned long long>(state.Playback.CurrentTick),
					state.ScalarPreviewValue
				);
				if (state.LastDiagnostic.Code != Status::Ok) {
					ImGui::TextWrapped("Current graph: %s", state.LastDiagnostic.Message.c_str());
					NavigateDiagnostic(state);
				}
				return;
			}
			if (!state.HaveGoodPreview || !state.CurrentTexture.IsValid()) {
				ImGui::TextDisabled("No successful preview yet.");
				if (state.LastDiagnostic.Code != Status::Ok)
					ImGui::TextWrapped("%s", state.LastDiagnostic.Message.c_str());
				return;
			}
			ImGui::Text(
				"%u x %u  RGBA8  %zu bytes  hash %016llx",
				state.Preview.Width,
				state.Preview.Height,
				state.Preview.Pixels.size(),
				static_cast<unsigned long long>(state.Preview.Hash)
			);
			if (state.LastDiagnostic.Code != Status::Ok) {
				ImGui::TextWrapped("Current graph: %s", state.LastDiagnostic.Message.c_str());
				NavigateDiagnostic(state);
			}
			if (!state.SinkMessage.empty()) ImGui::TextWrapped("%s", state.SinkMessage.c_str());
			const ImVec2 room = ImGui::GetContentRegionAvail();
			const float fit = std::min(
				1.0f,
				std::min(
					room.x / static_cast<float>(state.Preview.Width),
					room.y / static_cast<float>(state.Preview.Height)
				)
			);
			const ImVec2 size(
				std::max(1.0f, state.Preview.Width * fit), std::max(1.0f, state.Preview.Height * fit)
			);
			void *handle = renderer.TextureHandle(state.CurrentTexture);
			if (handle == nullptr) {
				ImGui::TextUnformatted("Preview texture is unavailable in the current renderer.");
				return;
			}
			ImGui::Image(reinterpret_cast<ImTextureID>(handle), size);
		}

	}

	void DrawImageComposer(engine::render::Renderer &renderer, bool &open) {
		State &state = Composer();
		Initialize(state);
		PumpRetiredTexture(state, renderer);

		ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
		if (!ImGui::Begin("Image Composer", &open, ImGuiWindowFlags_MenuBar)) {
			ImGui::End();
			ImGui::PopStyleColor(2);
			return;
		}
		if (AdvanceImageGraphPlayback(state.Playback, ImGui::GetIO().DeltaTime)) RequestPreview(state);
		FinishInactiveEdit(state);
		DrawToolbar(state);
		ImGui::Separator();

		const ImVec2 room = ImGui::GetContentRegionAvail();
		const float rightWidth = engine::ui::Scaled(310.0f);
		const float toolbarHeight = engine::ui::Scaled(28.0f);
		if (ImGui::BeginChild(
				"##image-graph-canvas", ImVec2(room.x - rightWidth, room.y - toolbarHeight), false
			)) {
			state.Canvas.Draw(state.Graph);
		}
		ImGui::EndChild();
		ImGui::SameLine();
		if (ImGui::BeginChild("##image-graph-inspector", ImVec2(0.0f, room.y - toolbarHeight), false)) {
			if (ImGui::BeginTabBar("##image-composer-tabs")) {
				if (ImGui::BeginTabItem("Nodes")) {
					DrawPalette(state);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Inspector")) {
					DrawInspector(state);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Groups")) {
					DrawGroups(state);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Preview")) {
					DrawOutputs(state);
					ImGui::Separator();
					RefreshPreview(state, renderer);
					DrawPreview(state, renderer);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Assets / Sinks")) {
					DrawAssetsAndSinks(state, renderer);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Timeline")) {
					DrawTimeline(state);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Diagnostics")) {
					RefreshPreview(state, renderer);
					DrawDiagnostics(state);
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}
		ImGui::EndChild();
		RefreshPreview(state, renderer);
		ImGui::End();
		ImGui::PopStyleColor(2);
	}
}
