#include "AudioWindowPanel.hpp"
#include "ImageComposerInternal.hpp"
#include "ImageGraphArrayEditor.hpp"
#include "ImageGraphChoices.hpp"
#include "ImageGraphDocumentEdit.hpp"
#include "ImageGraphExportTriggers.hpp"
#include "ImageGraphGroupHost.hpp"
#include "ImageGraphHost.hpp"
#include "ImageGraphInputs.hpp"
#include "ImageGraphObservations.hpp"
#include "ImageGraphPorts.hpp"
#include "ImageGraphPreview.hpp"
#include "ImageGraphSourceEdit.hpp"
#include "KeyframeKindEditor.hpp"
#include "TimelineDopesheet.hpp"
#include "TimelineKeyEditor.hpp"
#include "Vector2Panel.hpp"
#include "WavExport.hpp"
#include "WavTimelinePanel.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/bake/Pxcx.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/WavPreview.hpp>
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scripthost/ComposerLua.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
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
#include <studio/PxcxSave.hpp>
#include <studio/WavPreview.hpp>
#include <type_traits>
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
		constexpr size_t PREVIEW_MAXIMUM_BYTES = IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES;
		constexpr size_t MAXIMUM_HISTORY = 128;

		struct ExportGrant {
			std::string NodeId;
			std::array<char, 4096> Directory{}, ImageEncoder{}, VideoEncoder{};
			std::filesystem::path Root;
			std::string Message;
			bool Failed = false;
			std::vector<std::filesystem::path> RetainedFrames;
		};

		struct FileReadControls {
			std::string NodeId;
			std::array<char, 4096> File{}, Resource{};
			std::string Message;
		};

		struct State {
			bool Initialized = false;
			std::vector<ExportGrant> ExportGrants;
			detail::ImageGraphExportUpdate ExportUpdate;
			detail::ImageGraphObservations PcxObservations;
			std::chrono::steady_clock::time_point SessionStart = std::chrono::steady_clock::now();
			std::vector<engine::imagegraph::ComposerLuaMessage> LuaMessages;
			std::unique_ptr<engine::imagegraph::ComposerLuaHost> LuaHost =
				engine::script::MakeComposerLuaHost();
			detail::ImageGraphHost Host;
			std::vector<engine::imagegraphexport::GraphFileGrant> FileGrants;
			std::vector<FileReadControls> FileControls;
			bool LivePreview = true;
			bool PreviewDirty = true;
			bool CanvasNeedsReload = false;
			bool PreviewRequested = true;
			bool HaveGoodPreview = false;
			bool HaveScalarPreview = false;
			bool HaveValuePreview = false;
			engine::imagegraph::Value ValuePreview = false;
			bool HaveVector2Preview = false;
			engine::imagegraph::Vector2 Vector2Preview;
			Vector2Panel VectorControls;
			bool HaveArrayPreview = false;
			engine::imagegraph::ArrayValue ArrayPreview;
			bool HaveActiveEdit = false;
			bool ShowAdvancedDiagnostics = false;
			uint64_t DocumentRevision = 1;
			uint64_t EvaluationInputRevision = 1;
			ImageGraphWavPreview WavAudio;
			std::string WavAudioMessage;
			engine::imagegraph::WavExport WavExportArtifact;
			std::string WavExportMessage;
			WavTimelinePanel WavTimeline;
			AudioWindowPanel AudioWindow;
			uint64_t NextOutputId = 1;
			ImageGraphPlayback Playback;
			KeyframeKindEditor KeyKind;
			TimelineKeyEditor Keys;
			TimelineDopesheet Dopesheet;
			uint8_t TextureSlot = 1;
			engine::imagegraph::PortDirection GroupPortDirection = engine::imagegraph::PortDirection::Input;
			engine::imagegraph::ValueType GroupPortType = engine::imagegraph::ValueType::Image;
			uint64_t PreviewHash = 0;
			uint64_t PxcxThumbnailTextureHash = 0;
			uint32_t PreviewWidth = 0;
			uint32_t PreviewHeight = 0;
			size_t PreviewPixelBytes = 0;
			engine::imagegraph::SurfaceFormat PreviewSourceFormat =
				engine::imagegraph::SurfaceFormat::RGBA8Unorm;
			ImGuiID ActiveEditId = 0;
			char Search[96] = {};
			char GraphName[256] = {};
			char PxcxPath[4096] = {};
			char AudioCapturePath[4096] = {};
			char WavSourceId[4096] = {};
			char WavFilePath[4096] = {};
			std::string SelectedOutput;
			std::string BindingOutputPort;
			std::string SinkMessage;
			std::string AdapterError;
			std::string PxcxPathDisplay;
			std::string PxcxOpenError;
			std::string GraphIoMessage;
			std::string PxcxThumbnailMessage;
			std::string AudioCapturePathDisplay;
			std::string AudioCaptureMessage;
			std::string WavSourceMessage;
			std::string ValuePreviewPort;
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
			double ScalarPreviewValue = 0.0;
			std::vector<engine::imagegraph::AudioCaptureFrame> AudioFrames;
			std::vector<engine::imagegraph::AudioClipSource> AudioClips;
			Image PxcxReferenceThumbnail;
			ImageGraphPreviewCache PreviewCache;
			Diagnostic LastDiagnostic;
			ImageGraphHistory History{MAXIMUM_HISTORY};
			ImageGraphGroupHost GroupHost;
			engine::imagegraph::CapturedFeedbackHost FeedbackHost;
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
			std::erase_if(state.FileGrants, [&](const auto &grant) {
				return std::none_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [&](const auto &node) {
						return node.Id == grant.NodeId && detail::ImageGraphFileReadType(node.Type);
					}
				);
			});
			std::erase_if(state.FileControls, [&](const auto &control) {
				return std::none_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [&](const auto &node) {
						return node.Id == control.NodeId && detail::ImageGraphFileReadType(node.Type);
					}
				);
			});
			std::erase_if(state.ExportGrants, [&](const auto &grant) {
				return std::none_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [&](const auto &node) {
						return node.Id == grant.NodeId && node.Type == "pc.export";
					}
				);
			});
			if (state.LuaHost) state.LuaHost->Reset();
			state.Host.ResetFiles();
			state.LuaMessages.clear();
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

		detail::ImageGraphHost &HostFor(State &state) {
			state.Host.Lua = state.LuaHost.get();
			state.Host.Grants = state.FileGrants;
			return state.Host;
		}

		void BindObservations(State &state, engine::imagegraph::EvaluationRequest &request) {
			const auto frame = GetImageGraphFrame(state.Playback);
			const std::string project =
				std::filesystem::path(state.PxcxPathDisplay.empty() ? state.GraphName : state.PxcxPathDisplay)
					.stem()
					.string();
			if (!state.PcxObservations.Matches(state.DocumentRevision, frame, project)) {
				const auto now = std::time(nullptr);
				std::tm calendar{};
#ifdef _WIN32
				localtime_s(&calendar, &now);
#else
				localtime_r(&now, &calendar);
#endif
				const double elapsed =
					std::chrono::duration<double>(std::chrono::steady_clock::now() - state.SessionStart)
						.count();
				state.PcxObservations.Capture(state.DocumentRevision, frame, project, elapsed, calendar);
			}
			state.PcxObservations.Bind(request);
		}

		void ReloadCanvas(State &state);
		bool ReconcileSplitOutputs(
			State &state, Document &document, const engine::imagegraph::GroupReplayState *replay = nullptr
		) {
			std::vector<std::string> splitIds;
			for (const auto &node : document.Nodes)
				if (node.Type == "pc.array_split") splitIds.push_back(node.Id);
			if (splitIds.empty()) return true;
			engine::imagegraph::Plan plan;
			engine::imagegraph::Diagnostic error;
			if (engine::imagegraph::Compile(document, plan, error) != engine::imagegraph::Status::Ok)
				return true;
			engine::imagegraph::EvaluationRequest request;
			request.HostProvider = &HostFor(state);
			request.GroupReplay = replay;
			request.GroupAuthoringRevision = replay ? replay->AuthoringRevision() : 0;
			BindObservations(state, request);
			request.AudioFrames = state.AudioFrames;
			request.AudioClips = state.AudioClips;
			(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
			std::vector<std::pair<std::string, size_t>> resized;
			for (const auto &id : splitIds) {
				engine::imagegraph::EvaluationSnapshot snapshot;
				if (engine::imagegraph::EvaluateNodeInputs(document, plan, id, request, snapshot, error) !=
					engine::imagegraph::Status::Ok)
					continue;
				size_t count = 0, minimum = 0;
				for (const auto &input : snapshot.Values()) {
					if (input.Port == "minimum_outputs") {
						if (const auto *value = std::get_if<int64_t>(&input.Data); value && *value > 0)
							minimum = size_t(*value);
					}
					if (input.Port == "array") {
						if (const auto *array = std::get_if<engine::imagegraph::ArrayValue>(&input.Data))
							count = !array->Items.empty()	 ? array->Items.size()
									: !array->Nested.empty() ? array->Nested.size()
															 : array->Elements.size();
						else
							count = 1;
					}
				}
				for (const auto &image : snapshot.Images())
					if (image.Port == "array") ++count;
				for (const auto &array : snapshot.ImageArrays())
					if (array.Port == "array") count = array.Data.Items.size();
				count = std::max(count, minimum);
				const auto *node = FindNode(document, id);
				const auto current = node ? detail::ImageGraphOutputPorts(*node).size() : 0;
				if (count != current) resized.emplace_back(id, count);
			}
			for (const auto &[id, count] : resized)
				if (!SetImageGraphSplitOutputCount(document, id, count, state.LastDiagnostic, false))
					return false;
			if (!resized.empty()) state.CanvasNeedsReload = true;
			return true;
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
			if (!ReconcileSplitOutputs(state, updated)) return;
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
			if (!state.History.TryRecord(state.Authored, updated)) {
				state.LastDiagnostic = {
					Status::LimitExceeded, {}, {}, "Canvas undo transition exceeds its history budget"
				};
				state.CanvasNeedsReload = true;
				return;
			}
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
			state.CanvasNeedsReload = false;
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
			state.VectorControls.Attach(state.Canvas, state.Authored, state.Ids, state.History, [&state] {
				AuthoredDocumentChanged(state);
			});
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

		ExportGrant &GrantFor(State &state, std::string_view nodeId) {
			const auto found =
				std::find_if(state.ExportGrants.begin(), state.ExportGrants.end(), [&](const auto &grant) {
					return grant.NodeId == nodeId;
				});
			if (found != state.ExportGrants.end()) return *found;
			ExportGrant grant;
			grant.NodeId = std::string(nodeId);
			state.ExportGrants.push_back(std::move(grant));
			return state.ExportGrants.back();
		}

		void RunAuthoredExports(
			State &state, detail::ImageGraphExportEvent event, std::string_view explicitNode = {}
		) {
			std::vector<std::string> nodes;
			for (const auto &node : state.Authored.Nodes)
				if (node.Type == "pc.export" && (explicitNode.empty() || node.Id == explicitNode))
					nodes.push_back(node.Id);
			if (nodes.empty()) return;
			ENGINE_PROFILE_CAT("image composer exports", engine::core::ProfileCategory::Engine);
			engine::imagegraph::Plan plan;
			Diagnostic error;
			if (engine::imagegraph::Compile(state.Authored, plan, error) != Status::Ok) {
				for (const auto &id : nodes) {
					auto &grant = GrantFor(state, id);
					grant.Message = error.Message;
					grant.Failed = true;
				}
				return;
			}
			for (const auto &id : nodes) {
				auto &grant = GrantFor(state, id);
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				BindObservations(state, request);
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				if (!state.GroupHost.Prepare(state.Authored, plan, state.DocumentRevision, request, error) ||
					!state.FeedbackHost.Prepare(
						state.Authored,
						plan,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						request,
						error,
						engine::imagegraph::Limits::MaximumEvaluationBytes,
						state.SelectedOutput,
						id
					)) {
					grant.Message = error.Message;
					grant.Failed = true;
					continue;
				}
				engine::imagegraph::EvaluationSnapshot captured;
				const auto *snapshot = &state.FeedbackHost.Snapshot();
				if (snapshot->Values().empty()) {
					if (engine::imagegraph::EvaluateNodeInputs(
							state.Authored, plan, id, request, captured, error
						) != Status::Ok) {
						grant.Message = error.Message;
						grant.Failed = true;
						continue;
					}
					snapshot = &captured;
				}
				if (explicitNode.empty() && !detail::SourceExportTriggered(snapshot->Values(), event)) {
					grant.Failed = false;
					grant.Message.clear();
					continue;
				}
				if (grant.Root.empty()) {
					grant.Message = "Grant an export directory before running this node.";
					grant.Failed = true;
					continue;
				}
				engine::imagegraphexport::GraphExportSettings settings;
				settings.Input = state.PxcxPathDisplay.empty()
									 ? std::filesystem::path(state.GraphName).replace_extension(".graph")
									 : std::filesystem::path(state.PxcxPathDisplay);
				settings.Output = grant.Root;
				settings.ImageEncoder = std::filesystem::path(grant.ImageEncoder.data());
				settings.VideoEncoder = std::filesystem::path(grant.VideoEncoder.data());
				settings.NativeGif = true;
				std::string failure;
				std::vector<std::filesystem::path> retained;
				if (engine::imagegraphexport::ExportAuthoredGraphNode(
						state.Authored, plan, request, settings, id, failure, &retained
					)) {
					grant.Message = "Export complete.";
					grant.Failed = false;
				} else {
					grant.Message = std::move(failure);
					grant.Failed = true;
				}
				grant.RetainedFrames = std::move(retained);
			}
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
			RunAuthoredExports(state, detail::ImageGraphExportEvent::Save);
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
			state.ExportGrants.clear();
			state.FileGrants.clear();
			state.FileControls.clear();
			state.ExportUpdate = {};
			state.Playback.CurrentTick = 0;
			ApplyImageGraphTimeline(state.Authored, state.Playback);
			state.History.Clear();
			state.GroupHost.Clear();
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
			Diagnostic diagnostic;
			if (!LoadImageGraphAudioCapture(state.AudioFrames, state.PreviewCache, path, diagnostic)) {
				state.AudioCaptureMessage = diagnostic.Message;
				return false;
			}
			state.AudioCapturePathDisplay = path.string();
			++state.EvaluationInputRevision;
			state.AudioCaptureMessage =
				"loaded " + std::to_string(state.AudioFrames.size()) + " recorded frames";
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
			state.ExportGrants.clear();
			state.FileGrants.clear();
			state.FileControls.clear();
			state.ExportUpdate = {};
			state.Playback.CurrentTick = 0;
			ApplyImageGraphTimeline(state.Authored, state.Playback);
			AuthoredDocumentChanged(state);
			state.SelectedOutput =
				state.Authored.Outputs.empty() ? std::string{} : state.Authored.Outputs.front().Id;
			state.NextOutputId = 1;
			state.History.Clear();
			state.GroupHost.Clear();
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
				image.Height > PREVIEW_MAXIMUM_DIMENSION ||
				!engine::imagegraph::ValidSurfaceLayout(
					image, PREVIEW_MAXIMUM_DIMENSION, IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES
				) ||
				!engine::imagegraph::FiniteSurfaceSamples(image)) {
				state.SinkMessage = "preview exceeds the 128 by 128 texture budget";
				return false;
			}
			if (state.HaveGoodPreview && state.PreviewHash == image.Hash &&
				state.PreviewWidth == image.Width && state.PreviewHeight == image.Height &&
				state.PreviewSourceFormat == image.Format && state.PreviewPixelBytes == image.Pixels.size()) {
				return true;
			}

			std::vector<std::byte> displayPixels;
			if (!studio::detail::PrepareImageGraphPreviewRgba8(image, displayPixels) ||
				displayPixels.size() > PREVIEW_MAXIMUM_BYTES) {
				state.SinkMessage = "image cannot be represented within the RGBA8 display preview budget";
				return false;
			}
			engine::assets::TextureData texture;
			texture.Width = image.Width;
			texture.Height = image.Height;
			texture.Format = engine::assets::TextureFormat::RGBA8;
			texture.Pixels = std::move(displayPixels);

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
			state.PreviewPixelBytes = image.Pixels.size();
			state.PreviewSourceFormat = image.Format;
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
			state.HaveVector2Preview = false;
			state.HaveValuePreview = false;
			state.SinkMessage.clear();
			if (state.SelectedOutput.empty()) {
				state.HaveScalarPreview = false;
				state.HaveArrayPreview = false;
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
				state.HaveArrayPreview = false;
				state.LastDiagnostic = {
					Status::InvalidOutput, {}, {}, "selected output is missing from the authored document"
				};
				return;
			}
			const size_t outputIndex = static_cast<size_t>(selectedOutput - state.Authored.Outputs.begin());
			const Document &previewDocument = state.Authored;
			const Node *outputNode = FindNode(previewDocument, selectedOutput->NodeId);
			const auto outputPorts = outputNode ? detail::ImageGraphOutputPorts(*outputNode)
												: std::vector<engine::imagegraph::PortSchema>{};
			const engine::imagegraph::PortSchema *outputPort = nullptr;
			const auto foundOutput =
				std::find_if(outputPorts.begin(), outputPorts.end(), [&](const auto &port) {
					return port.Id == selectedOutput->Port;
				});
			if (foundOutput != outputPorts.end()) outputPort = &*foundOutput;
			if (outputPort == nullptr) {
				state.HaveScalarPreview = false;
				state.HaveArrayPreview = false;
				state.LastDiagnostic = {
					Status::InvalidOutput,
					selectedOutput->NodeId,
					selectedOutput->Port,
					"selected output port is not declared"
				};
				return;
			}
			const bool imageOutput = outputPort->Type == engine::imagegraph::ValueType::Image;
			if (!imageOutput && !detail::ImageGraphValuePreviewSupported(outputPort->Type)) {
				state.HaveScalarPreview = false;
				state.HaveArrayPreview = false;
				state.LastDiagnostic = {
					Status::UnsupportedExecution,
					selectedOutput->NodeId,
					selectedOutput->Port,
					"selected resource requires its dedicated preview panel"
				};
				return;
			}
			if (imageOutput) {
				state.HaveScalarPreview = false;
				state.HaveArrayPreview = false;
				if (const Image *cached = state.PreviewCache.Find(
						state.DocumentRevision,
						outputIndex,
						state.Playback.CurrentTick,
						state.Playback.Subframe,
						state.Playback.NegativeFrame
					)) {
					if (!UploadPreview(state, renderer, *cached)) return;
					state.LastDiagnostic = {};
					return;
				}
			} else {
				state.HaveGoodPreview = false;
				state.HaveScalarPreview = false;
				state.HaveArrayPreview = false;
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
					if (state.LuaHost) state.LuaHost->Reset();
					state.Host.ResetFiles();
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				BindObservations(state, request);
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				request.AudioFrames =
					std::span<const engine::imagegraph::AudioCaptureFrame>(state.AudioFrames);
				request.AudioClips = state.AudioClips;
				if (!state.GroupHost.Prepare(
						previewDocument, plan, state.DocumentRevision, request, diagnostic
					)) {
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				if (!state.FeedbackHost.Prepare(
						previewDocument,
						plan,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						request,
						diagnostic,
						engine::imagegraph::Limits::MaximumEvaluationBytes,
						state.SelectedOutput
					)) {
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				studio::ImageGraphPreviewValue preview;
				const auto *feedbackOutput = state.FeedbackHost.Value(state.SelectedOutput);
				if (feedbackOutput) {
					if (const auto *image = state.FeedbackHost.Output(state.SelectedOutput))
						preview = *image;
					else if (const auto *value =
								 std::get_if<engine::imagegraph::EvaluatedValue>(&feedbackOutput->Output))
						preview = *value;
					else {
						state.LastDiagnostic = {
							Status::InvalidOutput, {}, {}, "preview requires a single image"
						};
						return;
					}
				}
				const auto status =
					feedbackOutput
						? Status::Ok
						: studio::EvaluateImageGraphPreview(
							  previewDocument, plan, state.SelectedOutput, request, preview, diagnostic
						  );
				if (state.LuaHost) {
					auto messages = state.LuaHost->TakeMessages();
					if (!messages.empty()) state.LuaMessages = std::move(messages);
				}
				if (status != Status::Ok) {
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				if (auto *value = std::get_if<engine::imagegraph::EvaluatedValue>(&preview)) {
					if (auto *array = std::get_if<engine::imagegraph::ArrayValue>(&value->Data)) {
						if (array->ElementType == engine::imagegraph::ValueType::Scalar ||
							array->ElementType == engine::imagegraph::ValueType::Integer) {
							state.ArrayPreview = std::move(*array);
							state.ValuePreviewPort = value->Port;
							state.HaveArrayPreview = true;
							state.LastDiagnostic = {};
							return;
						}
					}
					if (const auto *vector = std::get_if<engine::imagegraph::Vector2>(&value->Data)) {
						state.Vector2Preview = *vector;
						state.ValuePreviewPort = value->Port;
						state.HaveVector2Preview = true;
						state.LastDiagnostic = {};
						return;
					}
					const auto *scalar = std::get_if<double>(&value->Data);
					if (scalar == nullptr) {
						state.ValuePreview = std::move(value->Data);
						state.ValuePreviewPort = value->Port;
						state.HaveValuePreview = true;
						state.LastDiagnostic = {};
						return;
					}
					state.ScalarPreviewValue = *scalar;
					state.ValuePreviewPort = value->Port;
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
					state.Playback.Subframe,
					state.Playback.NegativeFrame
				);
				state.LastDiagnostic = {};
			}
		}

		void ApplyHistory(State &state, bool redo) {
			const bool changed =
				redo ? state.History.Redo(state.Authored) : state.History.Undo(state.Authored);
			if (changed) {
				state.GroupHost.Clear();
				const auto frame = GetImageGraphFrame(state.Playback);
				ApplyImageGraphTimeline(state.Authored, state.Playback);
				(void)SetImageGraphAuthorFrame(state.Playback, frame);
				AuthoredDocumentChanged(state);
				ReloadCanvas(state);
			}
		}

		void ApplyDocumentEdit(State &state, const auto &edit) {
			if (ApplyImageGraphDocumentEdit(state.Authored, state.History, [&](Document &document) {
					if constexpr (std::is_same_v<std::invoke_result_t<decltype(edit), Document &>, bool>) {
						if (!edit(document)) return false;
					} else
						edit(document);
					return ReconcileSplitOutputs(state, document);
				}))
				AuthoredDocumentChanged(state);
			else if (state.GroupHost.Revision != state.DocumentRevision)
				// A refused staged transaction must rebuild previews from the retained document.
				state.GroupHost.Clear();
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
				if (state.History.TryRecord(before, fresh)) {
					state.Authored = std::move(fresh);
					state.ExportGrants.clear();
					state.FileGrants.clear();
					state.FileControls.clear();
					state.ExportUpdate = {};
					AuthoredDocumentChanged(state);
					state.SelectedOutput = "output-main";
					state.NextOutputId = 1;
					state.Playback = {};
					ApplyImageGraphTimeline(state.Authored, state.Playback);
					state.Canvas.Select(nodegraph::NO_NODE);
					ReloadCanvas(state);
				} else
					state.LastDiagnostic = {
						Status::LimitExceeded, {}, {}, "new graph cannot retain its undo snapshot"
					};
			}
			ImGui::SameLine();
			ImGui::Checkbox("Live preview", &state.LivePreview);
			ImGui::SameLine();
			if (ImGui::Button("Render")) {
				RequestPreview(state, true);
			}
			ImGui::SameLine();
			ImGui::Text(
				"frame %.20Lg", engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback))
			);
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

		void RecordPropertyEdit(State &state) {
			if (state.History.TryRecord(state.EditBefore, state.Authored)) return;
			state.Authored = std::move(state.EditBefore);
			state.GroupHost.Clear();
			AuthoredDocumentChanged(state);
			state.CanvasNeedsReload = true;
			state.LastDiagnostic = {
				Status::LimitExceeded, {}, {}, "Property undo transition exceeds its history budget"
			};
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
				RecordPropertyEdit(state);
				state.HaveActiveEdit = false;
				state.ActiveEditId = 0;
			}
		}

		void FinishInactiveEdit(State &state) {
			if (!state.HaveActiveEdit || GImGui->ActiveId == state.ActiveEditId) return;
			RecordPropertyEdit(state);
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

		void DrawProjectSettings(State &state) {
			if (!state.Authored.Project) {
				ImGui::TextUnformatted("Fresh-project defaults");
				ImGui::TextUnformatted("Surface 32 x 32");
				if (ImGui::Button("Author project settings")) {
					ApplyDocumentEdit(state, [&](Document &document) {
						SetImageGraphProjectSettings(document, {}, state.LastDiagnostic);
					});
				}
				return;
			}
			const engine::imagegraph::ProjectSettings &authored = *state.Authored.Project;
			std::optional<engine::imagegraph::ProjectSettings> edited;
			// Clone the palette only when a control edits it, not on every idle panel frame.
			const auto draft = [&]() -> engine::imagegraph::ProjectSettings & {
				if (!edited) edited = authored;
				return *edited;
			};
			std::array<uint32_t, 2> dimensions{authored.SurfaceWidth, authored.SurfaceHeight};
			if (ImGui::InputScalarN("Surface size", ImGuiDataType_U32, dimensions.data(), 2)) {
				draft().SurfaceWidth = dimensions[0];
				draft().SurfaceHeight = dimensions[1];
			}
			int64_t interpolation = authored.Interpolation;
			if (ImGui::InputScalar("Interpolation (0..6)", ImGuiDataType_S64, &interpolation))
				draft().Interpolation = interpolation;
			int64_t oversample = authored.Oversample;
			if (ImGui::InputScalar("Oversample (0..12)", ImGuiDataType_S64, &oversample))
				draft().Oversample = oversample;
			ImGui::Separator();
			ImGui::Text("Palette (%zu)", authored.Palette.size());
			for (size_t index = 0; index < authored.Palette.size(); index++) {
				ImGui::PushID(static_cast<int>(index));
				Colour colour = authored.Palette[index];
				if (DrawColourField("Colour", colour)) draft().Palette[index] = colour;
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					auto &palette = draft().Palette;
					palette.erase(palette.begin() + static_cast<std::ptrdiff_t>(index));
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			const bool paletteFull =
				authored.Palette.size() >= engine::imagegraph::Limits::MaximumProjectPaletteEntries;
			ImGui::BeginDisabled(paletteFull);
			if (ImGui::SmallButton("Add colour") && !paletteFull) {
				draft().Palette.push_back(Colour{});
			}
			ImGui::EndDisabled();
			if (ImGui::Button("Reset defaults")) {
				edited = engine::imagegraph::ProjectSettings{};
			}
			if (edited) {
				ApplyDocumentEdit(state, [&](Document &document) {
					SetImageGraphProjectSettings(document, *edited, state.LastDiagnostic);
				});
			}
			if (ImGui::Button("Remove project override")) {
				ApplyDocumentEdit(state, RemoveImageGraphProjectSettings);
				state.LastDiagnostic = {};
			}
			if (state.LastDiagnostic.Code != Status::Ok)
				ImGui::TextWrapped("%s", state.LastDiagnostic.Message.c_str());
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
			if (!colourPalette &&
				(array.ElementType == ValueType::Any || !array.Items.empty() || !array.Nested.empty()))
				return detail::DrawImageGraphSourceArray(array, state.ArrayPageOffset);
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
				default:
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

		bool DrawSourceChoiceValue(const engine::imagegraph::CatalogueInput &input, Value &replacement) {
			const imagegraph_choices::View choices{input};
			bool changed = false;
			if (choices.Count() != 0) {
				const auto selected = choices.Selected(replacement);
				const std::string preview = selected ? std::string(selected->Label) : "Custom value";
				if (ImGui::BeginCombo("##choice", preview.c_str())) {
					for (size_t row = 0; row < choices.Count(); ++row) {
						const auto entry = choices.At(row);
						if (entry->Separator) {
							ImGui::Separator();
							continue;
						}
						ImGui::PushID(entry->SourceIndex);
						const bool active = selected && selected->SourceIndex == entry->SourceIndex;
						if (ImGui::Selectable(std::string(entry->Label).c_str(), active)) {
							replacement = *choices.Select(row);
							changed = true;
						}
						if (active) ImGui::SetItemDefaultFocus();
						ImGui::PopID();
					}
					ImGui::EndCombo();
				}
			}
			if (input.SourceBehavior && input.SourceBehavior->FractionalInterpolation == true) {
				double value = imagegraph_choices::Number(replacement).value_or(0);
				if (ImGui::InputDouble("##value", &value, 0, 0, "%.17g")) {
					if (const auto edit = choices.Fraction(value)) {
						replacement = *edit;
						changed = true;
					}
				}
			} else {
				int64_t value = 0;
				if (const auto *choice = std::get_if<engine::imagegraph::EnumValue>(&replacement))
					value = choice->Value;
				else if (const auto *integer = std::get_if<int64_t>(&replacement))
					value = *integer;
				if (ImGui::InputScalar("##value", ImGuiDataType_S64, &value)) {
					replacement = engine::imagegraph::EnumValue{value};
					changed = true;
				}
			}
			return changed;
		}

		bool DrawValueWidget(State &state, std::string nodeId, AuthoredValue &property) {
			bool changed = false;
			Value replacement = property.Data;
			const Node *authored = FindNode(state.Authored, nodeId);
			const std::string editedPort = property.Port;
			const bool conditionalChannels =
				authored && (authored->Type == "pc.color_to_rgb" || authored->Type == "pc.color_to_hsv");
			const auto *choiceInput =
				authored ? imagegraph_choices::Input(authored->Type, property.Port) : nullptr;
			const auto *boundaryDeclaration = state.GroupHost.Replay.Find(nodeId);
			const bool triggerButton =
				(property.Port == "parent_value" && boundaryDeclaration &&
				 boundaryDeclaration->Domain.Kind == engine::imagegraph::SourceSocketKind::Trigger) ||
				(choiceInput && choiceInput->SourceKind == "Trigger");
			if (triggerButton) {
				changed = ImGui::Button("Trigger##value");
				if (changed) replacement = true;
			} else if (choiceInput && choiceInput->Type == engine::imagegraph::ValueType::Enum &&
					   imagegraph_choices::Number(replacement)) {
				changed = DrawSourceChoiceValue(*choiceInput, replacement);
			} else if (const auto *value = std::get_if<bool>(&property.Data)) {
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
			// A combo selection finishes before the numeric field becomes the last ImGui item.
			if (changed && !state.HaveActiveEdit) {
				state.EditBefore = state.Authored;
				state.HaveActiveEdit = true;
				state.ActiveEditId = itemId;
			}
			if (changed) {
				const Node *node = FindNode(state.Authored, nodeId);
				const bool boundary = node && node->Type == "pc.group_input";
				const bool sourceInput = node && choiceInput && choiceInput->SourceIndex >= 0;
				if (boundary || sourceInput || triggerButton) {
					engine::imagegraph::GroupRefreshEvent event;
					event.At.HostProvider = &HostFor(state);
					event.At.AudioFrames = state.AudioFrames;
					event.At.AudioClips = state.AudioClips;
					BindObservations(state, event.At);
					event.NodeId = std::string(nodeId);
					event.EditedPort = property.Port;
					event.LocalValue = &replacement;
					event.LocalAnimated =
						triggerButton || ImageGraphGroupHost::Mode(state.Authored, *node, property.Port) ==
											 engine::imagegraph::GroupSubtypeAnimator::Animated;
					event.SubtypeAnimator = ImageGraphGroupHost::Mode(state.Authored, *node, "subtype");
					event.Reason = property.Port == "parent_value"
									   ? engine::imagegraph::GroupRefreshReason::ParentEdit
									   : engine::imagegraph::GroupRefreshReason::Edit;
					(void)engine::imagegraph::SetFrameTime(event.At, GetImageGraphFrame(state.Playback));
					changed = state.GroupHost.Edit(
						state.Authored,
						state.History,
						state.DocumentRevision,
						event,
						state.LastDiagnostic,
						false
					);
				} else {
					changed = SetImageGraphValue(
						state.Authored, nodeId, property.Port, std::move(replacement), state.LastDiagnostic
					);
				}
			}
			if (changed && (editedPort == "minimum_outputs" || editedPort == "array")) {
				(void)ReconcileSplitOutputs(state, state.Authored);
				ReloadCanvas(state);
			}
			if (changed && editedPort == "output_array" && conditionalChannels) {
				const auto ports = detail::ImageGraphOutputPorts(*FindNode(state.Authored, nodeId));
				const auto valid = [&](std::string_view id) {
					return std::any_of(ports.begin(), ports.end(), [&](const auto &port) {
						return port.Id == id;
					});
				};
				std::erase_if(state.Authored.Links, [&](const auto &link) {
					return link.FromNode == nodeId && !valid(link.FromPort);
				});
				std::erase_if(state.Authored.Outputs, [&](const auto &output) {
					return output.NodeId == nodeId && !valid(output.Port);
				});
				ReloadCanvas(state);
			}
			EndPropertyEdit(state, itemId, changed);
			return changed;
		}

		const char *ValueTypeLabel(engine::imagegraph::ValueType type) {
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
			case ValueType::Path3D:
				return "Path3D";
			case ValueType::PixelBox:
				return "PixelBox";
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
			default:
				// Runtime-only sockets use their durable type name.
				return engine::imagegraph::ValueTypeName(type).data();
			}
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
			default:
				// Images, meshes, audio and runtime-only sockets have no authored default.
				return std::nullopt;
			}
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

		void DrawReadOnlyValue(State &state, Value &value, size_t depth, size_t &remaining);
		void DrawReadOnlyElement(
			State &state, engine::imagegraph::ElementValue &element, size_t depth, size_t &remaining
		) {
			std::visit(
				[&](auto &leaf) {
					using T = std::decay_t<decltype(leaf)>;
					if constexpr (std::is_same_v<T, std::string>) {
						if (!remaining) return;
						--remaining;
						ImGui::TextUnformatted(leaf.data(), leaf.data() + leaf.size());
					} else if constexpr (std::is_same_v<T, engine::imagegraph::StructValue>) {
						if (!remaining || depth > 32) return;
						--remaining;
						if (leaf.Data)
							for (auto &[name, field] : leaf.Data->Fields) {
								if (!remaining) break;
								ImGui::PushID(name.c_str());
								ImGui::TextUnformatted(name.c_str());
								DrawReadOnlyValue(state, field, depth + 1, remaining);
								ImGui::PopID();
							}
					} else if constexpr (std::is_same_v<T, engine::imagegraph::MatrixValue>) {
						ImGui::Text("Matrix %u x %u", leaf.Rows, leaf.Columns);
						for (const double entry : leaf.Values) {
							if (!remaining) break;
							--remaining;
							ImGui::Text("%.9g", entry);
						}
					} else if constexpr (requires { leaf.Data; } ||
										 std::is_same_v<T, engine::imagegraph::AudioBit>) {
						if (remaining) --remaining;
						ImGui::TextUnformatted("Owned resource payload");
					} else {
						Value value = leaf;
						DrawReadOnlyValue(state, value, depth, remaining);
					}
				},
				element
			);
		}
		void DrawReadOnlyArrayItem(
			State &state, engine::imagegraph::SourceArrayItem &item, size_t depth, size_t &remaining
		) {
			if (!remaining || depth > 32) return;
			if (auto *leaf = std::get_if<engine::imagegraph::ElementValue>(&item.Data)) {
				DrawReadOnlyElement(state, *leaf, depth, remaining);
			} else if (auto *children =
						   std::get_if<std::vector<engine::imagegraph::SourceArrayItem>>(&item.Data)) {
				--remaining;
				if (ImGui::TreeNode("##array", "Array (%zu)", children->size())) {
					for (size_t i = 0; i < children->size() && remaining; ++i) {
						ImGui::PushID(static_cast<int>(i));
						DrawReadOnlyArrayItem(state, (*children)[i], depth + 1, remaining);
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
			} else if (const auto *image = std::get_if<engine::imagegraph::Image>(&item.Data)) {
				--remaining;
				ImGui::Text("Surface %u x %u", image->Width, image->Height);
			}
		}
		void DrawReadOnlyValue(State &state, Value &value, size_t depth, size_t &remaining) {
			if (!remaining || depth > 32) return;
			--remaining;
			if (auto *array = std::get_if<engine::imagegraph::ArrayValue>(&value)) {
				ImGui::Text("Array %s", engine::imagegraph::ValueTypeName(array->ElementType).data());
				for (size_t i = 0; i < array->Elements.size() && remaining; ++i) {
					ImGui::PushID(static_cast<int>(i));
					DrawReadOnlyElement(state, array->Elements[i], depth + 1, remaining);
					ImGui::PopID();
				}
				for (size_t row = 0; row < array->Nested.size() && remaining; ++row) {
					ImGui::PushID(static_cast<int>(row));
					if (ImGui::TreeNode("row", "Row %zu", row)) {
						for (size_t i = 0; i < array->Nested[row].size() && remaining; ++i) {
							ImGui::PushID(static_cast<int>(i));
							DrawReadOnlyElement(state, array->Nested[row][i], depth + 1, remaining);
							ImGui::PopID();
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				for (size_t i = 0; i < array->Items.size() && remaining; ++i) {
					ImGui::PushID(static_cast<int>(i));
					DrawReadOnlyArrayItem(state, array->Items[i], depth + 1, remaining);
					ImGui::PopID();
				}
			} else if (auto *matrix = std::get_if<engine::imagegraph::MatrixValue>(&value)) {
				ImGui::Text("Matrix %u x %u", matrix->Rows, matrix->Columns);
				for (const double entry : matrix->Values) {
					if (!remaining) break;
					--remaining;
					ImGui::Text("%.9g", entry);
				}
			} else if (auto *structure = std::get_if<engine::imagegraph::StructValue>(&value)) {
				if (structure->Data)
					for (auto &[name, field] : structure->Data->Fields) {
						if (!remaining) break;
						ImGui::PushID(name.c_str());
						ImGui::TextUnformatted(name.c_str());
						DrawReadOnlyValue(state, field, depth + 1, remaining);
						ImGui::PopID();
					}
			} else if (const auto *path = std::get_if<engine::imagegraph::PathValue3D>(&value)) {
				ImGui::Text("Path3D: %zu anchors", path->Data ? path->Data->Anchors.size() : 0);
			} else if (const auto *box = std::get_if<engine::imagegraph::PixelBoxValue>(&value)) {
				if (box->Data) {
					const auto &bounds =
						box->Data->FixedBounds ? *box->Data->FixedBounds : box->Data->BaseBounds;
					ImGui::Text(
						"PixelBox: %.9g, %.9g to %.9g, %.9g", bounds[0], bounds[1], bounds[2], bounds[3]
					);
				} else
					ImGui::TextUnformatted("PixelBox: empty");
			} else if (std::holds_alternative<engine::imagegraph::AudioBit>(value) ||
					   std::holds_alternative<engine::imagegraph::MeshValue3D>(value) ||
					   std::holds_alternative<engine::imagegraph::MeshValue2D>(value) ||
					   std::holds_alternative<engine::imagegraph::SceneValue3D>(value) ||
					   std::holds_alternative<engine::imagegraph::MaterialValue3D>(value) ||
					   std::holds_alternative<engine::imagegraph::LightValue3D>(value)) {
				ImGui::TextUnformatted("Owned resource payload");
			} else {
				ImGui::BeginDisabled();
				(void)DrawDynamicDefault(state, value);
				ImGui::EndDisabled();
			}
		}

		void DrawDynamicInputs(State &state, Node &node, const engine::imagegraph::NodeSchema &schema) {
			if (!schema.DynamicInputs) return;
			ImGui::Separator();
			ImGui::TextUnformatted("Instance inputs");
			const auto *catalogue = engine::imagegraph::FindCatalogueEntry(node.Type);
			const bool grouped =
				catalogue && catalogue->DynamicGroupLength > 0 && !catalogue->DynamicTemplate.empty();
			if (grouped) {
				int count = 0;
				for (const auto &input : node.DynamicInputs) {
					size_t group = 0;
					if (engine::imagegraph::FindDynamicTemplate(*catalogue, input.Id, group))
						count = std::max(count, int(group + 1));
				}
				if (ImGui::InputInt("Input groups", &count)) {
					const std::string nodeId = node.Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						return count >= 0 && SetSourceImageGraphDynamicGroupCount(
												 document, nodeId, size_t(count), state.LastDiagnostic
											 );
					});
					ReloadCanvas(state);
					return;
				}
			}
			const std::vector<engine::imagegraph::DynamicInput> inputs = node.DynamicInputs;
			for (const auto &source : inputs) {
				auto input = source;
				const auto *sourceTemplate = imagegraph_choices::Input(node.Type, input.Id);
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
				if (node.Type == "pc.gmroom") {
					std::string layer = input.SourceLayerName;
					if (ImGui::InputText(
							"Room layer",
							layer.data(),
							layer.capacity() + 1,
							ImGuiInputTextFlags_CallbackResize,
							ResizeTextInput,
							&layer
						)) {
						input.SourceLayerName = std::move(layer);
						changed = true;
					}
				}
				ImGui::BeginDisabled(grouped);
				if (ImGui::BeginCombo("##type", ValueTypeLabel(input.Type))) {
					for (const auto type : types) {
						const bool selected = input.Type == type;
						if (ImGui::Selectable(ValueTypeLabel(type), selected)) {
							input.Type = type;
							input.Default = DefaultForType(type);
							changed = true;
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::EndDisabled();
				if (grouped && (input.Type == engine::imagegraph::ValueType::Any ||
								detail::SourceLuaArgumentType(node, input.Id))) {
					std::optional<engine::imagegraph::ValueType> literalType;
					if (input.Default)
						for (const auto type : types) {
							const auto candidate = DefaultForType(type);
							if (candidate && candidate->index() == input.Default->index()) {
								literalType = type;
								break;
							}
						}
					if (ImGui::BeginCombo(
							"Literal type", literalType ? ValueTypeLabel(*literalType) : "None"
						)) {
						for (const auto type : types) {
							if (type == engine::imagegraph::ValueType::Image) continue;
							if (ImGui::Selectable(ValueTypeLabel(type), literalType == type)) {
								input.Default = DefaultForType(type);
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
				}
				if (input.Type != engine::imagegraph::ValueType::Image) {
					bool hasDefault = input.Default.has_value();
					// Physical source controls retain an animator value even while linked.
					ImGui::BeginDisabled(sourceTemplate && sourceTemplate->SourceIndex >= 0 && hasDefault);
					if (ImGui::Checkbox("Default", &hasDefault)) {
						input.Default = hasDefault ? DefaultForType(
														 input.Type == engine::imagegraph::ValueType::Any
															 ? engine::imagegraph::ValueType::Scalar
															 : input.Type
													 )
												   : std::nullopt;
						changed = true;
					}
					ImGui::EndDisabled();
					if (input.Default) {
						const auto choices = sourceTemplate ? std::optional<imagegraph_choices::View>(
																  imagegraph_choices::View{*sourceTemplate}
															  )
															: std::nullopt;
						if (choices && choices->Count()) {
							const auto selected = choices->Selected(*input.Default);
							const std::string label = selected ? std::string(selected->Label) : "Unknown";
							if (ImGui::BeginCombo("Value", label.c_str())) {
								for (size_t row = 0; row < choices->Count(); ++row) {
									const auto choice = choices->At(row);
									if (choice->Separator) {
										ImGui::Separator();
										continue;
									}
									const std::string name(choice->Label);
									if (ImGui::Selectable(
											name.c_str(),
											selected && selected->SourceIndex == choice->SourceIndex
										)) {
										input.Default = choices->Select(row);
										changed = true;
									}
								}
								ImGui::EndCombo();
							}
						} else
							changed = DrawDynamicDefault(state, *input.Default) || changed;
					}
				}
				if (grouped && sourceTemplate && sourceTemplate->SourceIndex >= 0 && input.Default &&
					ImGui::SmallButton("Key current frame")) {
					engine::imagegraph::GroupRefreshEvent event;
					event.NodeId = node.Id;
					event.EditedPort = input.Id;
					event.LocalValue = &*input.Default;
					event.LocalAnimated = true;
					event.At.HostProvider = &HostFor(state);
					event.At.AudioFrames = state.AudioFrames;
					event.At.AudioClips = state.AudioClips;
					BindObservations(state, event.At);
					(void)engine::imagegraph::SetFrameTime(event.At, GetImageGraphFrame(state.Playback));
					if (state.GroupHost.Edit(
							state.Authored, state.History, state.DocumentRevision, event, state.LastDiagnostic
						)) {
						AuthoredDocumentChanged(state);
						ReloadCanvas(state);
					}
					ImGui::PopID();
					return;
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(grouped || node.DynamicInputs.size() <= 1);
				const bool remove = ImGui::SmallButton("Remove");
				ImGui::EndDisabled();
				if (changed) {
					const std::string nodeId = node.Id;
					engine::imagegraph::EvaluationRequest request;
					request.HostProvider = &HostFor(state);
					request.AudioFrames = state.AudioFrames;
					request.AudioClips = state.AudioClips;
					BindObservations(state, request);
					(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
					if (ApplyImageGraphSourceDynamicInput(
							state.Authored,
							state.History,
							state.GroupHost,
							state.DocumentRevision,
							nodeId,
							input,
							request,
							state.LastDiagnostic,
							[&](Document &document, ImageGraphGroupHost &host) {
								return ReconcileSplitOutputs(state, document, &host.Replay);
							}
						))
						AuthoredDocumentChanged(state);
					ReloadCanvas(state);
					ImGui::PopID();
					return;
				}
				if (remove) {
					const std::string nodeId = node.Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						if (RemoveImageGraphDynamicInput(document, nodeId, input.Id, state.LastDiagnostic))
							state.LastDiagnostic = {};
					});
					ReloadCanvas(state);
					ImGui::PopID();
					return;
				}
				ImGui::PopID();
			}
			if (grouped) return;
			ImGui::BeginDisabled(
				node.DynamicInputs.size() >= engine::imagegraph::MaximumDynamicInputsForType(node.Type)
			);
			if (ImGui::SmallButton("Add input") &&
				node.DynamicInputs.size() < engine::imagegraph::MaximumDynamicInputsForType(node.Type)) {
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
			const auto quaternionMode = hasTrack ? track->QuaternionMode : std::optional<int64_t>{};
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
			const bool quaternionKeys = std::any_of(
				state.Authored.Keyframes.begin(), state.Authored.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == nodeId && key.Port == propertyId &&
						   std::holds_alternative<engine::imagegraph::Quaternion>(key.Data);
				}
			);
			if (quaternionKeys) {
				static constexpr const char *modes[]{"Unspecified", "Raw", "Euler degrees"};
				const int selected = quaternionMode ? static_cast<int>(*quaternionMode) + 1 : 0;
				if (ImGui::BeginCombo("Quaternion", modes[std::clamp(selected, 0, 2)])) {
					for (int choice = 0; choice < 3; choice++) {
						if (ImGui::Selectable(modes[choice], choice == selected)) {
							ApplyDocumentEdit(state, [&](Document &document) {
								SetImageGraphTrackQuaternionMode(
									document,
									nodeId,
									propertyId,
									choice == 0 ? std::nullopt : std::optional<int64_t>{choice - 1},
									state.LastDiagnostic
								);
							});
						}
					}
					ImGui::EndCombo();
				}
			}
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

		void DrawFileGrants(State &state, std::string_view nodeId) {
			auto found =
				std::find_if(state.FileControls.begin(), state.FileControls.end(), [&](const auto &control) {
					return control.NodeId == nodeId;
				});
			if (found == state.FileControls.end()) {
				FileReadControls controls;
				controls.NodeId = nodeId;
				state.FileControls.push_back(std::move(controls));
				found = std::prev(state.FileControls.end());
			}
			auto &controls = *found;
			ImGui::InputTextWithHint(
				"##read-file", "Exact file path", controls.File.data(), controls.File.size()
			);
			ImGui::InputTextWithHint(
				"##read-resource",
				"Resource key (blank for primary)",
				controls.Resource.data(),
				controls.Resource.size()
			);
			const auto changed = [&] {
				state.Host.RefreshFile(nodeId);
				state.PreviewCache.Clear();
				if (++state.EvaluationInputRevision == 0) state.EvaluationInputRevision = 1;
				RequestPreview(state, true);
			};
			if (ImGui::Button("Grant read")) {
				const std::filesystem::path file(controls.File.data());
				std::string resource(controls.Resource.data());
				const auto *node = FindNode(state.Authored, nodeId);
				if (node && (node->Type == "pc.image_sequence" || node->Type == "pc.image_animated") &&
					resource.empty())
					resource = file.string();
				if (file.empty())
					controls.Message = "Enter an exact file path.";
				else if (!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle)
							  .AllowsName(file.string()))
					controls.Message = "File type is disabled by content policy.";
				else {
					auto grant =
						std::find_if(state.FileGrants.begin(), state.FileGrants.end(), [&](const auto &item) {
							return item.NodeId == nodeId && item.Resource == resource;
						});
					if (grant != state.FileGrants.end()) {
						grant->File = file;
						controls.Message.clear();
						changed();
					} else if (state.FileGrants.size() >= 256 ||
							   std::count_if(
								   state.FileGrants.begin(), state.FileGrants.end(), [&](const auto &item) {
									   return item.NodeId == nodeId;
								   }
							   ) >= 128)
						controls.Message = "Read grants exceed the session limit.";
					else {
						state.FileGrants.push_back({std::string(nodeId), file, false, resource});
						controls.Message.clear();
						changed();
					}
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Read / refresh")) {
				changed();
				engine::imagegraph::Plan plan;
				Diagnostic error;
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				BindObservations(state, request);
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
				engine::imagegraph::HostNodeCapture capture;
				if (engine::imagegraph::Compile(state.Authored, plan, error) != Status::Ok ||
					!state.GroupHost.Prepare(state.Authored, plan, state.DocumentRevision, request, error) ||
					!state.FeedbackHost.Prepare(
						state.Authored,
						plan,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						request,
						error,
						engine::imagegraph::Limits::MaximumEvaluationBytes,
						state.SelectedOutput
					))
					controls.Message = error.Message;
				else if (engine::imagegraphexport::ExecuteGraphHostNode(
							 state.Authored, plan, request, nodeId, capture, controls.Message
						 ))
					controls.Message = "Read captured.";
			}
			if (ImGui::Button("Revoke reads")) {
				std::erase_if(state.FileGrants, [&](const auto &grant) { return grant.NodeId == nodeId; });
				controls.Message.clear();
				changed();
			}
			for (const auto &grant : state.FileGrants)
				if (grant.NodeId == nodeId)
					ImGui::TextWrapped(
						"%s: %s",
						grant.Resource.empty() ? "Primary" : grant.Resource.c_str(),
						grant.File.string().c_str()
					);
			if (!controls.Message.empty()) ImGui::TextWrapped("%s", controls.Message.c_str());
		}

		void DrawExportGrant(State &state, std::string_view nodeId) {
			auto &grant = GrantFor(state, nodeId);
			ImGui::InputTextWithHint(
				"##export-directory-grant",
				"Export directory grant",
				grant.Directory.data(),
				grant.Directory.size()
			);
			if (ImGui::Button("Grant directory")) {
				std::error_code error;
				const std::filesystem::path entered(grant.Directory.data());
				if (entered.empty())
					grant.Message = "Enter an export directory.";
				else {
					const auto absolute = std::filesystem::absolute(entered, error);
					const auto root =
						error ? std::filesystem::path{} : std::filesystem::weakly_canonical(absolute, error);
					if (!error) std::filesystem::create_directories(root, error);
					if (error)
						grant.Message = error.message();
					else {
						grant.Root = root;
						grant.Message.clear();
					}
				}
			}
			if (!grant.Root.empty()) {
				ImGui::SameLine();
				if (ImGui::Button("Revoke")) {
					grant.Root.clear();
					grant.Message.clear();
				}
				ImGui::TextWrapped("Granted: %s", grant.Root.string().c_str());
			}
			ImGui::InputTextWithHint(
				"##export-image-encoder",
				"Image encoder executable (optional)",
				grant.ImageEncoder.data(),
				grant.ImageEncoder.size()
			);
			ImGui::InputTextWithHint(
				"##export-video-encoder",
				"Video encoder executable (optional)",
				grant.VideoEncoder.data(),
				grant.VideoEncoder.size()
			);
			ImGui::BeginDisabled(grant.Root.empty());
			if (ImGui::Button("Export"))
				RunAuthoredExports(state, detail::ImageGraphExportEvent::Update, nodeId);
			ImGui::EndDisabled();
			if (!grant.Message.empty()) ImGui::TextWrapped("%s", grant.Message.c_str());
			for (const auto &path : grant.RetainedFrames)
				ImGui::TextWrapped("Retained frames: %s", path.string().c_str());
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
			if (node->Type == "pc.verlet_sim_mesh_cache" && ImGui::Button("Cache Mesh")) {
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				BindObservations(state, request);
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				const std::string_view capture(nodeId);
				request.SimulationCacheCaptures = std::span<const std::string_view>(&capture, 1);
				engine::imagegraph::Plan plan;
				if (engine::imagegraph::Compile(state.Authored, plan, state.LastDiagnostic) == Status::Ok &&
					state.GroupHost.Prepare(
						state.Authored, plan, state.DocumentRevision, request, state.LastDiagnostic
					) &&
					state.FeedbackHost.Prepare(
						state.Authored,
						plan,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						request,
						state.LastDiagnostic,
						engine::imagegraph::Limits::MaximumEvaluationBytes,
						state.SelectedOutput
					)) {
					state.LastDiagnostic = {};
					RequestPreview(state, true);
				}
			}
			if (node->Type == "pc.export") DrawExportGrant(state, nodeId);
			if (detail::ImageGraphFileReadType(node->Type) &&
				!detail::ImageGraphFileUsesOwnedContent(node->Type))
				DrawFileGrants(state, nodeId);
			if (schema->Properties.empty() && !schema->DynamicInputs) {
				ImGui::TextDisabled("No authored properties.");
				return;
			}
			ImGui::Separator();
			if (node->Type == "pc.audio_window") {
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				BindObservations(state, request);
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
				engine::imagegraph::Diagnostic error;
				if (state.AudioWindow.Update(
						state.Authored,
						nodeId,
						request,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						error
					))
					state.AudioWindow.Draw();
				else
					ImGui::TextWrapped("%s", error.Message.c_str());
			}
			if (node->Type == "pc.wav_file_read") {
				if (ImGui::Button("Sync length")) {
					engine::imagegraph::EvaluationRequest request;
					request.HostProvider = &HostFor(state);
					BindObservations(state, request);
					(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
					request.AudioFrames = state.AudioFrames;
					request.AudioClips = state.AudioClips;
					request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
					ApplyDocumentEdit(state, [&](Document &document) {
						return SyncImageGraphWavTimeline(
							document, nodeId, request, state.Playback, state.LastDiagnostic
						);
					});
					ApplyImageGraphTimeline(state.Authored, state.Playback);
					node = FindNode(state.Authored, nodeId);
					if (!node) return;
				}
				ImGui::TextWrapped("File watching is unavailable.");
				engine::imagegraph::EvaluationRequest waveformRequest;
				waveformRequest.HostProvider = &HostFor(state);
				BindObservations(state, waveformRequest);
				(void)engine::imagegraph::SetFrameTime(waveformRequest, GetImageGraphFrame(state.Playback));
				waveformRequest.AudioClips = state.AudioClips;
				waveformRequest.AudioFrames = state.AudioFrames;
				waveformRequest.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
				engine::imagegraph::Diagnostic waveformError;
				if (state.WavTimeline.Update(
						state.Authored,
						nodeId,
						waveformRequest,
						state.Playback.FramesPerSecond,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						waveformError
					))
					state.WavTimeline.Draw();
				else
					ImGui::TextWrapped("%s", waveformError.Message.c_str());
			}
			if (node->Type == "pc.wav_file_write") {
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				BindObservations(state, request);
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
				DrawImageGraphWavExport(
					state.Authored, nodeId, request, state.WavExportArtifact, state.WavExportMessage
				);
			}
			const auto *vectorPresentation =
				node->Type == "pc.vector2" ? state.VectorControls.Presentation(nodeId) : nullptr;
			const auto visibleVectorProperty = [&](std::string_view property) {
				if (!vectorPresentation) return true;
				if (property == "gizmo_shape") return vectorPresentation->Style == 1;
				if (property == "gizmo_sprite") return vectorPresentation->Style == 2;
				if (property == "gizmo_size") return vectorPresentation->Style != 0;
				return true;
			};
			for (AuthoredValue &property : node->Values) {
				if (!visibleVectorProperty(property.Port)) continue;
				if (node->Type == "pc.wav_file_read" && property.Port == "sync_length") continue;
				ImGui::PushID(node->Id.c_str());
				ImGui::PushID(property.Port.c_str());
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(property.Port.c_str());
				ImGui::SameLine(92.0f);
				ImGui::SetNextItemWidth(-1.0f);
				const auto revision = state.DocumentRevision;
				if (DrawValueWidget(state, node->Id, property) || state.DocumentRevision != revision) {
					ImGui::PopID();
					ImGui::PopID();
					return;
				}
				DrawAnimationTrackControls(state, *node, property.Port);
				ImGui::PopID();
				ImGui::PopID();
				if (state.DocumentRevision != revision) return;
			}
			for (const engine::imagegraph::PropertySchema &property : schema->Properties) {
				if (!visibleVectorProperty(property.Id)) continue;
				if (node->Type == "pc.wav_file_read" && property.Id == "sync_length") continue;
				if (FindValue(*node, property.Id) != nullptr) continue;
				const std::optional<Value> initial =
					ImageGraphPropertyDefault(state.Authored, node->Type, property.Id);
				const std::string propertyId(property.Id);
				ImGui::PushID(node->Id.c_str());
				ImGui::PushID(propertyId.c_str());
				ImGui::TextDisabled("%s: value missing", propertyId.c_str());
				ImGui::SameLine(92.0f);
				ImGui::BeginDisabled(!initial.has_value());
				const bool initialize = ImGui::SmallButton("Initialize") && initial.has_value();
				if (initialize) {
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
				if (initialize) return;
			}
			if (schema->DynamicOutputs && node->Type == "pc.array_split") {
				int outputs = static_cast<int>(detail::ImageGraphOutputPorts(*node).size());
				if (ImGui::InputInt("Outputs", &outputs) && outputs >= 0 && outputs <= 4096) {
					const std::string id = node->Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						(void)SetImageGraphSplitOutputCount(
							document, id, size_t(outputs), state.LastDiagnostic
						);
					});
					ReloadCanvas(state);
					return;
				}
			}
			DrawDynamicInputs(state, *node, *schema);
		}

		void DrawOutputs(State &state) {
			const auto *selected = FindNode(state.Authored, SelectedNodeId(state));
			const auto available = selected ? detail::ImageGraphOutputPorts(*selected)
											: std::vector<engine::imagegraph::PortSchema>{};
			if (!available.empty()) {
				if (std::none_of(available.begin(), available.end(), [&](const auto &port) {
						return port.Id == state.BindingOutputPort;
					}))
					state.BindingOutputPort = available.front().Id;
				if (ImGui::BeginCombo("Output port", state.BindingOutputPort.c_str())) {
					for (const auto &port : available) {
						const std::string id(port.Id);
						if (ImGui::Selectable(id.c_str(), state.BindingOutputPort == id))
							state.BindingOutputPort = id;
					}
					ImGui::EndCombo();
				}
			}

			const std::string capturedId = SelectedNodeId(state);
			const Node *captured = FindNode(state.Authored, capturedId);
			if (captured && captured->Type == "image.captured" && !state.SelectedOutput.empty()) {
				if (ImGui::Button("Use selected output as feedback"))
					ApplyDocumentEdit(state, [&](Document &document) {
						SetImageGraphValue(
							document,
							capturedId,
							"source_id",
							std::string("feedback:") + state.SelectedOutput,
							state.LastDiagnostic
						);
					});
				ImGui::TextUnformatted("Feedback starts with a transparent image at project size.");
			}
			if (state.FeedbackHost.Active() && ImGui::Button("Reset feedback preview")) {
				state.FeedbackHost.Clear();
				state.PreviewCache.Clear();
				RequestPreview(state, true);
			}

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
						std::find_if(available.begin(), available.end(), [&](const auto &entry) {
							return entry.Id == state.BindingOutputPort &&
								   (entry.Type == engine::imagegraph::ValueType::Image ||
									detail::ImageGraphValuePreviewSupported(entry.Type));
						});
					if (port != available.end()) {
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
			const auto bindingPorts = bindingNode ? detail::ImageGraphOutputPorts(*bindingNode)
												  : std::vector<engine::imagegraph::PortSchema>{};
			const engine::imagegraph::PortSchema *bindingPort = nullptr;
			const auto foundBinding =
				std::find_if(bindingPorts.begin(), bindingPorts.end(), [&](const auto &entry) {
					return entry.Id == state.BindingOutputPort;
				});
			if (foundBinding != bindingPorts.end()) bindingPort = &*foundBinding;
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
					if (output) {
						ports.clear();
						for (const auto &port : detail::ImageGraphOutputPorts(*node))
							ports.emplace_back(port.Id);
					}
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
					return;
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
					ImGui::EndDisabled();
					return;
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
				if (!state.ImportedPxcx && ImGui::BeginCombo("Type", ValueTypeLabel(state.GroupPortType))) {
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
						if (ImGui::Selectable(ValueTypeLabel(type), active)) state.GroupPortType = type;
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
						if (state.ImportedPxcx
								? AddSourceImageGraphGroupPort(
									  document, groupId, portId, port.Direction, state.LastDiagnostic
								  )
								: AddImageGraphGroupPort(
									  document, groupId, port, junction, state.LastDiagnostic
								  ))
							state.LastDiagnostic = {};
					});
					ReloadCanvas(state);
					ImGui::EndDisabled();
					return;
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
						junction == state.Authored.Junctions.end() ? "unknown"
																   : ValueTypeLabel(junction->Type)
					);
					ImGui::SameLine();
					if (ImGui::SmallButton("Remove socket")) {
						const std::string groupId = groupNow->Id;
						const std::string junctionId = port.JunctionId;
						const std::string controlId = port.ControlNodeId;
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
							if (!controlId.empty()) {
								std::erase_if(document.Nodes, [&](const auto &node) {
									return node.Id == controlId;
								});
								std::erase_if(document.Outputs, [&](const auto &output) {
									return output.NodeId == controlId;
								});
								std::erase_if(document.Keyframes, [&](const auto &key) {
									return key.NodeId == controlId;
								});
								std::erase_if(document.Tracks, [&](const auto &track) {
									return track.NodeId == controlId;
								});
							}
							std::erase_if(document.Links, [&](const auto &item) {
								return item.FromNode == junctionId || item.ToNode == junctionId ||
									   (!controlId.empty() &&
										(item.FromNode == controlId || item.ToNode == controlId));
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
						document,
						nodeId,
						port,
						state.Playback.CurrentTick,
						"step",
						state.LastDiagnostic,
						state.Playback.Subframe,
						state.Playback.NegativeFrame
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
			const auto sine = frame.SineDriver;
			if (ImGui::SmallButton(sine ? "Sine..." : "Add sine")) {
				if (!sine) {
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

			auto driver = sine.value_or(engine::imagegraph::KeyframeSineDriver{});
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

		void DrawKeyframeSourceDriver(State &state, size_t index) {
			using namespace engine::imagegraph;
			if (state.Authored.Keyframes[index].SineDriver) {
				DrawKeyframeSineDriver(state, index, state.Authored.Keyframes[index]);
				return;
			}
			static constexpr const char *names[]{
				"None", "Linear", "Snap", "Bounce", "Elastic", "Curve", "Sine", "Captured audio"
			};
			const auto &authored = state.Authored.Keyframes[index].SourceDriver;
			const size_t choice = authored ? authored->index() + 1 : 0;
			if (ImGui::SmallButton(names[choice])) ImGui::OpenPopup("##source-driver");
			if (!ImGui::BeginPopup("##source-driver")) return;
			if (ImGui::BeginCombo("Driver", names[choice])) {
				for (size_t kind = 0; kind < std::size(names); kind++) {
					if (!ImGui::Selectable(names[kind], kind == choice)) continue;
					std::optional<KeyframeSourceDriver> driver;
					switch (kind) {
					case 1:
						driver = KeyframeLinearDriver{};
						break;
					case 2:
						driver = KeyframeSnapDriver{};
						break;
					case 3:
						driver = KeyframeBounceDriver{};
						break;
					case 4:
						driver = KeyframeElasticDriver{};
						break;
					case 5:
						driver = KeyframeCurveDriver{};
						break;
					case 6:
						driver = KeyframeSineDriver{};
						break;
					case 7:
						driver = KeyframeAudioDriver{};
						break;
					default:
						break;
					}
					ApplyDocumentEdit(state, [&](Document &document) {
						SetImageGraphKeyframeSourceDriver(document, index, driver, state.LastDiagnostic);
					});
				}
				ImGui::EndCombo();
			}
			// Curve data is copied only while its controls are open, never for an idle timeline row.
			auto driver = state.Authored.Keyframes[index].SourceDriver;
			if (driver) {
				const bool changed = std::visit(
					[&](auto &control) {
						using Control = std::decay_t<decltype(control)>;
						if constexpr (std::is_same_v<Control, KeyframeLinearDriver>) {
							return ImGui::InputDouble("Speed", &control.Speed);
						} else if constexpr (std::is_same_v<Control, KeyframeSnapDriver>) {
							return ImGui::InputDouble("Size", &control.Size);
						} else if constexpr (std::is_same_v<Control, KeyframeBounceDriver> ||
											 std::is_same_v<Control, KeyframeElasticDriver>) {
							bool edited = ImGui::InputScalar("Amount", ImGuiDataType_S64, &control.Amount);
							edited |= ImGui::InputDouble("Spacing", &control.Spacing);
							edited |= ImGui::InputDouble("Curve", &control.Curve);
							return edited;
						} else if constexpr (std::is_same_v<Control, KeyframeAudioDriver>) {
							std::array<char, 256> source{};
							std::copy_n(
								control.SourceId.data(),
								std::min(control.SourceId.size(), source.size() - 1),
								source.data()
							);
							bool edited = ImGui::InputText("Capture source", source.data(), source.size());
							if (edited) control.SourceId = source.data();
							if (ImGui::BeginCombo("Metric", control.Metric.c_str())) {
								for (const char *metric : {"rms", "peak", "mean"})
									if (ImGui::Selectable(metric, control.Metric == metric)) {
										control.Metric = metric;
										edited = true;
									}
								ImGui::EndCombo();
							}
							ImGui::TextUnformatted("Native captured-audio offset at exact tick");
							edited |= ImGui::InputScalar("Channel", ImGuiDataType_U32, &control.Channel);
							edited |= ImGui::InputDouble("Gain", &control.Gain);
							edited |= ImGui::InputDouble("Bias", &control.Bias);
							return edited;
						} else if constexpr (std::is_same_v<Control, KeyframeCurveDriver>) {
							return DrawCurveValue(control.Data);
						} else {
							bool edited = ImGui::InputDouble("Frequency", &control.Frequency);
							edited |= ImGui::InputDouble("Amplitude", &control.Amplitude);
							edited |= ImGui::InputDouble("Phase", &control.Phase);
							edited |= ImGui::InputDouble("Smooth", &control.Smooth);
							return edited;
						}
					},
					*driver
				);
				if (changed)
					ApplyDocumentEdit(state, [&](Document &document) {
						SetImageGraphKeyframeSourceDriver(document, index, driver, state.LastDiagnostic);
					});
				if (ImGui::SmallButton("Remove driver")) {
					ApplyDocumentEdit(state, [&](Document &document) {
						SetImageGraphKeyframeSourceDriver(
							document, index, std::nullopt, state.LastDiagnostic
						);
					});
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}

		void DrawKeyframeKind(State &state, size_t index) {
			state.KeyKind.Draw(state.Authored, index, [&] {
				bool accepted = false;
				ApplyDocumentEdit(state, [&](Document &document) {
					accepted = state.KeyKind.Commit(document, state.LastDiagnostic);
				});
				return accepted;
			});
		}

		void DrawTimeline(State &state) {
			if (ImGui::Button(state.Playback.Playing ? "Pause" : "Play")) {
				if (!state.Playback.Playing &&
					(state.Playback.NegativeFrame || state.Playback.CurrentTick < state.Playback.StartTick ||
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
				if (!state.Playback.Playing &&
					std::any_of(
						state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [](const Node &node) {
							return node.Type == "pc.wav_file_read";
						}
					)) {
					Diagnostic diagnostic;
					if (!state.WavAudio.Enable(diagnostic))
						state.WavAudioMessage = diagnostic.Message;
					else
						state.WavAudioMessage.clear();
				}
				state.Playback.Playing = !state.Playback.Playing;
				state.Playback.Accumulator = 0.0;
			}
			ImGui::SameLine();
			if (ImGui::Button("Step -1") &&
				(state.Playback.CurrentTick > state.Playback.StartTick || state.Playback.Subframe > 0.0)) {
				(void)SetImageGraphPlaybackFrame(
					state.Playback,
					static_cast<double>(
						engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback))
					) - 1.0
				);
				state.Playback.Direction = -1;
				RequestPreview(state);
			}
			ImGui::SameLine();
			if (ImGui::Button("Step +1") && state.Playback.CurrentTick < state.Playback.EndTick) {
				(void)SetImageGraphPlaybackFrame(
					state.Playback,
					static_cast<double>(
						engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback))
					) + 1.0
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
				static_cast<double>(engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback)));
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
			const bool cursorChanged =
				rangeChanged && SetImageGraphPlaybackFrame(state.Playback, previousFrame);
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
			double frame =
				static_cast<double>(engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback)));
			if (ImGui::InputDouble("##timeline-frame", &frame, 0.01, 1.0, "%.6f")) {
				if (SeekImageGraphAuthorFrame(
						state.Playback, frame, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyAlt
					))
					RequestPreview(state);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl: outside range. Alt: fractional frame.");
			ImGui::SameLine();
			if (ImGui::Button("Previous key")) {
				if (const auto key =
						PreviousImageGraphKey(state.Authored, GetImageGraphFrame(state.Playback));
					key && SetImageGraphAuthorFrame(state.Playback, *key))
					RequestPreview(state);
			}
			ImGui::SameLine();
			if (ImGui::Button("Next key")) {
				if (const auto key = NextImageGraphKey(state.Authored, GetImageGraphFrame(state.Playback));
					key && SetImageGraphAuthorFrame(state.Playback, *key))
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
					if (ImGui::Button("Remove at frame")) {
						ApplyDocumentEdit(state, [&](Document &document) {
							if (!RemoveImageGraphKeyframe(
									document,
									node->Id,
									value.Port,
									state.Playback.CurrentTick,
									state.LastDiagnostic,
									state.Playback.Subframe,
									state.Playback.NegativeFrame
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
			state.Dopesheet.Draw(
				state.Authored,
				state.DocumentRevision,
				state.Keys,
				GetImageGraphFrame(state.Playback),
				state.LastDiagnostic,
				[&] {
					bool accepted = false;
					ApplyDocumentEdit(state, [&](Document &document) {
						accepted = state.Dopesheet.Commit(document, state.Keys, state.LastDiagnostic);
					});
					return accepted;
				}
			);
			state.Keys.Draw(state.Authored, GetImageGraphFrame(state.Playback), state.LastDiagnostic, [&] {
				bool accepted = false;
				ApplyDocumentEdit(state, [&](Document &document) {
					accepted = state.Keys.Commit(document, state.LastDiagnostic);
				});
				return accepted;
			});
			if (ImGui::BeginTable("##keyframes", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn("Property");
				ImGui::TableSetupColumn("Frame");
				ImGui::TableSetupColumn("Interpolation");
				ImGui::TableSetupColumn("Ease in");
				ImGui::TableSetupColumn("Ease out");
				ImGui::TableSetupColumn("Driver");
				ImGui::TableSetupColumn("Kind");
				ImGui::TableSetupColumn("Action");
				ImGui::TableHeadersRow();
				for (size_t index = 0; index < state.Authored.Keyframes.size(); index++) {
					const Keyframe &frame = state.Authored.Keyframes[index];
					const std::string interpolation = frame.Interpolation;
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					if (state.Keys.DrawRow(frame)) {
						if (SetImageGraphAuthorFrame(state.Playback, engine::imagegraph::GetFrameTime(frame)))
							RequestPreview(state);
						if (const auto found = state.Ids.ToCanvas.find(frame.NodeId);
							found != state.Ids.ToCanvas.end()) {
							state.Canvas.Select(found->second);
							state.Canvas.Centre(state.Graph, found->second);
						}
					}
					ImGui::TableSetColumnIndex(1);
					ImGui::Text(
						"%.20Lg", engine::imagegraph::FrameTimeToReal(engine::imagegraph::GetFrameTime(frame))
					);
					ImGui::TableSetColumnIndex(2);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::BeginCombo("##interpolation", interpolation.c_str())) {
						for (const char *choice : {"step", "linear", "cubic", "source"}) {
							const bool selected = interpolation == choice;
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
					DrawKeyframeEaseSide(state, index, state.Authored.Keyframes[index], true);
					ImGui::PopID();
					ImGui::PopID();
					ImGui::TableSetColumnIndex(4);
					ImGui::PushID(static_cast<int>(index));
					ImGui::PushID("ease-out");
					DrawKeyframeEaseSide(state, index, state.Authored.Keyframes[index], false);
					ImGui::PopID();
					ImGui::PopID();
					ImGui::TableSetColumnIndex(5);
					ImGui::PushID(static_cast<int>(index));
					DrawKeyframeSourceDriver(state, index);
					ImGui::PopID();
					ImGui::TableSetColumnIndex(6);
					ImGui::PushID(static_cast<int>(index));
					DrawKeyframeKind(state, index);
					ImGui::PopID();
					ImGui::TableSetColumnIndex(7);
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
			ImGui::TextUnformatted("WAV sources");
			ImGui::InputTextWithHint(
				"##wav-source-name",
				"Source name matching the node path",
				state.WavSourceId,
				sizeof(state.WavSourceId)
			);
			ImGui::InputTextWithHint(
				"##wav-file-path", "WAV file path", state.WavFilePath, sizeof(state.WavFilePath)
			);
			if (ImGui::Button("Load or replace WAV")) {
				Diagnostic diagnostic;
				if (state.WavAudio.LoadSource(
						state.AudioClips, state.PreviewCache, state.WavSourceId, state.WavFilePath, diagnostic
					)) {
					state.WavSourceMessage = "loaded " + std::string(state.WavSourceId);
					++state.EvaluationInputRevision;
					state.LastDiagnostic = {};
					RequestPreview(state, true);
				} else {
					state.WavSourceMessage = diagnostic.Message;
				}
			}
			if (!state.WavSourceMessage.empty()) ImGui::TextWrapped("%s", state.WavSourceMessage.c_str());
			for (size_t index = 0; index < state.AudioClips.size();) {
				const auto &source = state.AudioClips[index];
				const auto &data = source.Data;
				const size_t channels = data.Channels.empty() ? 1 : data.Channels.size();
				const size_t samples =
					data.Channels.empty() ? data.Samples.size() : data.Channels.front().size();
				ImGui::PushID(source.SourceId.c_str());
				ImGui::TextWrapped(
					"%s | %.0f Hz | %zu channels | %.3f s",
					source.SourceId.c_str(),
					data.SampleRate,
					channels,
					data.SampleRate > 0 ? samples / data.SampleRate : 0
				);
				const bool remove = ImGui::Button("Remove WAV");
				ImGui::PopID();
				if (remove) {
					Diagnostic diagnostic;
					if (state.WavAudio.RemoveSource(
							state.AudioClips, state.PreviewCache, source.SourceId, diagnostic
						)) {
						++state.EvaluationInputRevision;
						RequestPreview(state, true);
					} else {
						state.WavSourceMessage = diagnostic.Message;
						++index;
					}
				} else
					index++;
			}
			ImGui::Separator();
			ImGui::TextUnformatted("Recorded audio");
			ImGui::InputTextWithHint(
				"##image-audio-capture-path",
				"Path to audio-capture 1 document",
				state.AudioCapturePath,
				sizeof(state.AudioCapturePath)
			);
			if (ImGui::Button("Load audio capture")) OpenAudioCapture(state);
			ImGui::SameLine();
			if (ImGui::Button("Clear audio capture")) {
				ClearImageGraphAudioCapture(state.AudioFrames, state.PreviewCache);
				++state.EvaluationInputRevision;
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
			if (state.ImportedPxcx) {
				ImGui::SameLine();
				if (ImGui::Button("Save PXCX")) {
					Diagnostic diagnostic;
					if (!SavePxcxProjection(
							std::filesystem::path(state.PxcxPath),
							*state.ImportedPxcx,
							state.Authored,
							GetImageGraphFrame(state.Playback),
							diagnostic
						))
						state.PxcxOpenError = diagnostic.Message;
					else {
						state.PxcxOpenError.clear();
						RunAuthoredExports(state, detail::ImageGraphExportEvent::Save);
					}
				}
			}
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
			for (const auto &message : state.LuaMessages) {
				ImGui::Text("%s %s", message.Warning ? "Script warning" : "Script", message.NodeId.c_str());
				ImGui::TextUnformatted(message.Text.c_str());
			}
			for (const auto &grant : state.ExportGrants) {
				if (!grant.Failed) continue;
				ImGui::PushID(grant.NodeId.c_str());
				ImGui::TextWrapped("Export %s: %s", grant.NodeId.c_str(), grant.Message.c_str());
				if (ImGui::SmallButton("Select export node")) {
					const auto node = state.Ids.ToCanvas.find(grant.NodeId);
					if (node != state.Ids.ToCanvas.end()) state.Canvas.Select(node->second);
				}
				ImGui::PopID();
			}
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
				IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES
			);
			ImGui::TextUnformatted("Profiling: image composer preview appears in the F5 frame graph.");
			if (!state.SinkMessage.empty()) ImGui::TextWrapped("Texture sink: %s", state.SinkMessage.c_str());
			ImGui::Checkbox("Show diagnostic details", &state.ShowAdvancedDiagnostics);
			if (state.ShowAdvancedDiagnostics && state.LastDiagnostic.Code != Status::Ok) {
				ImGui::Text("Status code: %u", static_cast<unsigned>(state.LastDiagnostic.Code));
			}
		}

		void DrawArraySamples(const std::vector<engine::imagegraph::ElementValue> &samples) {
			if (samples.empty()) {
				ImGui::TextUnformatted("Empty");
				return;
			}
			if (!ImGui::BeginTable("##samples", 2, ImGuiTableFlags_BordersInnerV)) return;
			ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(48.0f));
			ImGui::TableSetupColumn("Value");
			ImGui::TableHeadersRow();
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(samples.size()));
			while (clipper.Step()) {
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("%d", index);
					ImGui::TableNextColumn();
					const auto &sample = samples[static_cast<size_t>(index)];
					if (const auto *integer = std::get_if<int64_t>(&sample))
						ImGui::Text("%lld", static_cast<long long>(*integer));
					else if (const auto *number = std::get_if<double>(&sample))
						ImGui::Text("%.17g", *number);
				}
			}
			ImGui::EndTable();
		}

		void DrawPreview(State &state, engine::render::Renderer &renderer) {
			state.VectorControls.DrawSnapControls();
			if (!state.VectorControls.Diagnostic().Message.empty())
				ImGui::TextWrapped("%s", state.VectorControls.Diagnostic().Message.c_str());
			if (state.HaveVector2Preview) {
				ImGui::Text(
					"Vector output %s  [%.17g, %.17g]",
					state.ValuePreviewPort.c_str(),
					state.Vector2Preview.X,
					state.Vector2Preview.Y
				);
				const engine::imagegraph::ProjectSettings defaults;
				const auto &project = state.Authored.Project ? *state.Authored.Project : defaults;
				const auto room = ImGui::GetContentRegionAvail();
				const double scale = std::min(
					{1., double(room.x) / project.SurfaceWidth, double(room.y) / project.SurfaceHeight}
				);
				if (scale > 0) {
					const auto origin = ImGui::GetCursorScreenPos();
					ImGui::Dummy({float(project.SurfaceWidth * scale), float(project.SurfaceHeight * scale)});
					state.VectorControls.DrawOverlay(
						renderer,
						{origin.x, origin.y, project.SurfaceWidth * scale, project.SurfaceHeight * scale},
						scale,
						SelectedNodeId(state)
					);
				}
				return;
			}

			if (state.HaveArrayPreview) {
				ImGui::Text(
					"Array output %s  frame %.20Lg",
					state.ValuePreviewPort.c_str(),
					engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback))
				);
				if (state.ArrayPreview.Nested.empty())
					DrawArraySamples(state.ArrayPreview.Elements);
				else {
					for (size_t channel = 0; channel < state.ArrayPreview.Nested.size(); channel++) {
						ImGui::PushID(static_cast<int>(channel));
						if (ImGui::TreeNodeEx(
								"##channel",
								ImGuiTreeNodeFlags_DefaultOpen,
								"Channel %zu (%zu samples)",
								channel,
								state.ArrayPreview.Nested[channel].size()
							)) {
							DrawArraySamples(state.ArrayPreview.Nested[channel]);
							ImGui::TreePop();
						}
						ImGui::PopID();
					}
				}
				return;
			}

			if (state.HaveValuePreview) {
				ImGui::Text("Output %s", state.ValuePreviewPort.c_str());
				size_t remaining = engine::imagegraph::Limits::MaximumArrayElements;
				DrawReadOnlyValue(state, state.ValuePreview, 0, remaining);
				if (!remaining) ImGui::TextUnformatted("Display limit reached");
				return;
			}
			if (state.HaveScalarPreview) {
				ImGui::Text(
					"Scalar output %s  frame %.20Lg  value %.17g",
					state.ValuePreviewPort.c_str(),
					engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback)),
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
				"%u x %u  source %.*s (%zu bytes)  display RGBA8  hash %016llx",
				state.PreviewWidth,
				state.PreviewHeight,
				static_cast<int>(
					engine::imagegraph::DescribeSurfaceFormat(state.PreviewSourceFormat)->Name.size()
				),
				engine::imagegraph::DescribeSurfaceFormat(state.PreviewSourceFormat)->Name.data(),
				state.PreviewPixelBytes,
				static_cast<unsigned long long>(state.PreviewHash)
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
					room.x / static_cast<float>(state.PreviewWidth),
					room.y / static_cast<float>(state.PreviewHeight)
				)
			);
			const ImVec2 size(
				std::max(1.0f, state.PreviewWidth * fit), std::max(1.0f, state.PreviewHeight * fit)
			);
			void *handle = renderer.TextureHandle(state.CurrentTexture);
			if (handle == nullptr) {
				ImGui::TextUnformatted("Preview texture is unavailable in the current renderer.");
				return;
			}
			ImGui::Image(reinterpret_cast<ImTextureID>(handle), size);
			const auto origin = ImGui::GetItemRectMin();
			state.VectorControls.DrawOverlay(
				renderer, {origin.x, origin.y, size.x, size.y}, fit, SelectedNodeId(state)
			);
		}

	}

	void CloseImageComposerVector2Preview(engine::render::Renderer &renderer) {
		Composer().VectorControls.Close(renderer);
	}

	void CloseImageComposerAudioPreview() {
		State &state = Composer();
		state.Playback.Playing = false;
		state.WavAudio.Close();
		state.WavAudioMessage.clear();
	}

	void DrawImageComposer(engine::render::Renderer &renderer, bool &open) {
		State &state = Composer();
		Initialize(state);
		PumpRetiredTexture(state, renderer);
		state.VectorControls.PumpRetired(renderer);

		if (AdvanceImageGraphPlayback(state.Playback, ImGui::GetIO().DeltaTime)) RequestPreview(state);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
		if (!ImGui::Begin("Image Composer", &open, ImGuiWindowFlags_MenuBar)) {
			state.VectorControls.FinishPointer(
				ImGui::IsMouseDown(ImGuiMouseButton_Left), ImGui::IsMouseDown(ImGuiMouseButton_Middle)
			);
			ImGui::End();
			ImGui::PopStyleColor(2);
			if (!open) {
				if (state.LuaHost) state.LuaHost->Reset();
				state.Host.ResetFiles();
				state.ExportUpdate = {};
				CloseImageComposerAudioPreview();
				state.VectorControls.Close(renderer);
			} else {
				Diagnostic diagnostic;
				if (!state.WavAudio.Update(
						state.Authored,
						state.AudioClips,
						state.AudioFrames,
						state.Playback,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						diagnostic
					))
					state.WavAudioMessage = diagnostic.Message;
				else if (state.WavAudio.Enabled())
					state.WavAudioMessage.clear();
			}
			return;
		}
		FinishInactiveEdit(state);
		DrawToolbar(state);
		engine::imagegraph::EvaluationRequest vectorRequest;
		vectorRequest.HostProvider = &HostFor(state);
		BindObservations(state, vectorRequest);
		(void)engine::imagegraph::SetFrameTime(vectorRequest, GetImageGraphFrame(state.Playback));
		vectorRequest.AudioFrames = state.AudioFrames;
		vectorRequest.AudioClips = state.AudioClips;
		vectorRequest.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
		state.VectorControls.Refresh(
			state.Authored, vectorRequest, state.DocumentRevision, state.EvaluationInputRevision
		);
		ImGui::Separator();

		const ImVec2 room = ImGui::GetContentRegionAvail();
		const float rightWidth = engine::ui::Scaled(310.0f);
		const float toolbarHeight = engine::ui::Scaled(28.0f);
		if (ImGui::BeginChild(
				"##image-graph-canvas", ImVec2(room.x - rightWidth, room.y - toolbarHeight), false
			)) {
			if (state.CanvasNeedsReload) ReloadCanvas(state);
			state.Canvas.Draw(state.Graph);
			if (state.CanvasNeedsReload) ReloadCanvas(state);
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
				if (ImGui::BeginTabItem("Project")) {
					DrawProjectSettings(state);
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
		if (state.ExportUpdate.Accept(
				state.DocumentRevision, state.EvaluationInputRevision, GetImageGraphFrame(state.Playback)
			))
			RunAuthoredExports(state, detail::ImageGraphExportEvent::Update);
		Diagnostic audioDiagnostic;
		if (!state.WavAudio.Update(
				state.Authored,
				state.AudioClips,
				state.AudioFrames,
				state.Playback,
				state.DocumentRevision,
				state.EvaluationInputRevision,
				audioDiagnostic
			))
			state.WavAudioMessage = audioDiagnostic.Message;
		else if (state.WavAudio.Enabled())
			state.WavAudioMessage.clear();
		if (!state.WavAudioMessage.empty()) ImGui::TextWrapped("%s", state.WavAudioMessage.c_str());
		state.VectorControls.FinishPointer(
			ImGui::IsMouseDown(ImGuiMouseButton_Left), ImGui::IsMouseDown(ImGuiMouseButton_Middle)
		);
		ImGui::End();
		if (!open) {
			if (state.LuaHost) state.LuaHost->Reset();
			state.Host.ResetFiles();
			state.ExportUpdate = {};
			CloseImageComposerAudioPreview();
			state.VectorControls.Close(renderer);
		}
		ImGui::PopStyleColor(2);
	}
}
