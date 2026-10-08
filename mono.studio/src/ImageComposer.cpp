#include "AnimationTrackPolicyEditor.hpp"
#include "AudioWindowPanel.hpp"
#include "ImageComposerInternal.hpp"
#include "ImageComposerPanels.hpp"
#include "ImageGraphAnimationControl.hpp"
#include "ImageGraphAppendProject.hpp"
#include "ImageGraphArguments.hpp"
#include "ImageGraphArrayEditor.hpp"
#include "ImageGraphArtworkEdit.hpp"
#include "ImageGraphAxisControls.hpp"
#include "ImageGraphCacheBackground.hpp"
#include "ImageGraphCacheClearAction.hpp"
#include "ImageGraphCacheControls.hpp"
#include "ImageGraphCacheEditObservation.hpp"
#include "ImageGraphCanvasInputs.hpp"
#include "ImageGraphChoices.hpp"
#include "ImageGraphComposerCadence.hpp"
#include "ImageGraphComposerExports.hpp"
#include "ImageGraphComposerHost.hpp"
#include "ImageGraphCookAction.hpp"
#include "ImageGraphDocumentEdit.hpp"
#include "ImageGraphExportIntent.hpp"
#include "ImageGraphExportTriggers.hpp"
#include "ImageGraphFilePublish.hpp"
#include "ImageGraphFontArtifact.hpp"
#include "ImageGraphFontBindings.hpp"
#include "ImageGraphGroupHost.hpp"
#include "ImageGraphHistoryCanvas.hpp"
#include "ImageGraphHistoryKeys.hpp"
#include "ImageGraphHistorySource.hpp"
#include "ImageGraphHlslGroups.hpp"
#include "ImageGraphHost.hpp"
#include "ImageGraphImageActions.hpp"
#include "ImageGraphImageEdit.hpp"
#include "ImageGraphInputs.hpp"
#include "ImageGraphNoiseControls.hpp"
#include "ImageGraphObservations.hpp"
#include "ImageGraphPorts.hpp"
#include "ImageGraphPreview.hpp"
#include "ImageGraphPreviewProvider.hpp"
#include "ImageGraphPreviewResult.hpp"
#include "ImageGraphPreviewSequence.hpp"
#include "ImageGraphRasterNoiseControls.hpp"
#include "ImageGraphRigid.hpp"
#include "ImageGraphRigidMeshAction.hpp"
#include "ImageGraphSourceEdit.hpp"
#include "ImageGraphSourceKeyEdit.hpp"
#include "ImageGraphSourceTimelineTransition.hpp"
#include "ImageGraphTimelineRead.hpp"
#include "ImagePreviewPanel.hpp"
#include "KeyframeKindEditor.hpp"
#include "KeyframeSourceDriverControls.hpp"
#include "TimelineDopesheet.hpp"
#include "TimelineEaseEditor.hpp"
#include "TimelineKeyDelete.hpp"
#include "TimelineKeyEditor.hpp"
#include "TimelineRegions.hpp"
#include "TimelineScalarKeys.hpp"
#include "Vector2Panel.hpp"
#include "WavExport.hpp"
#include "WavFileChecker.hpp"
#include "WavTimelinePanel.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/bake/Pxcx.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/SourceCommonDispatch.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraph/WavPreview.hpp>
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphexport/GraphExportSession.hpp>
#include <engine/imagegraphexport/PxcxCollectionLoad.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scripthost/ComposerLua.hpp>
#include <engine/ui/Metrics.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
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
#include <studio/ImageComposerArguments.hpp>
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
			std::array<char, 4096> File{}, Resource{}, Directory{}, Template{};
			std::string Message;
			int CacheLayout = 0;
		};

		struct FontInputControls {
			std::array<char, 4096> Artifact{}, DefaultPath{};
			std::array<char, 4096> Directory{}, ApplicationLocation{}, ProjectPath{};
			std::array<char, 256> Alias{}, AliasPath{}, GrantNode{};
			std::array<char, 4096> GrantPath{};
			std::string Message;
			bool PendingFontArtifactLoad = false, PendingFontArtifactSave = false;
			bool PendingClearFontObservations = false, PendingApplyFontInputs = false;
		};

		struct SourceCollectionHostState {
			std::string OwnerId;
			std::string SourceType;
			std::string GroupId;
			std::vector<std::string> Members;
			bool RefreshNodesPending = false;
			bool RefreshNodeDisplayPending = false;
		};

		struct State {
			bool Initialized = false;
			detail::ImageComposerPanels Panels;
			std::vector<ExportGrant> ExportGrants;
			detail::ImageGraphExportUpdate ExportUpdate;
			detail::ImageGraphObservations PcxObservations;
			std::chrono::steady_clock::time_point SessionStart = std::chrono::steady_clock::now();
			std::vector<engine::imagegraph::ComposerLuaMessage> LuaMessages;
			std::unique_ptr<engine::imagegraph::ComposerLuaHost> LuaHost =
				engine::script::MakeComposerLuaHost();
			detail::ImageGraphHost Host;
			detail::ImageGraphComposerCadence ComposerCadence;
			detail::ImageGraphPreviewObservations PreviewObservations;
			detail::ImageGraphPreviewSequence PreviewSequence;
			detail::ImageGraphComposerExports ComposerExports;
			detail::ImageGraphExportIntent ExportIntent;
			std::unique_ptr<engine::imagegraphexport::GraphExportSession> RangeExport;
			uint64_t ExportIntentGeneration = 0;
			std::vector<engine::core::Name> ComposerCaptureNames;
			bool ComposerDevicePending = false;
			std::vector<engine::imagegraphexport::GraphFileGrant> FileGrants;
			std::vector<engine::imagegraphexport::GraphDirectoryGrant> DirectoryGrants;
			std::vector<engine::imagegraphexport::GraphImageCacheLayoutObservation> ImageCacheLayouts;
			std::vector<FileReadControls> FileControls;
			std::unique_ptr<engine::imagegraphfont::GraphFontInputs> FontInputs =
				std::make_unique<engine::imagegraphfont::GraphFontInputs>();
			engine::imagegraphfont::GraphFontConfiguration EditableFontConfiguration;
			std::optional<engine::imagegraphfont::GraphFontConfiguration> PendingFontConfiguration;
			FontInputControls FontControls;
			bool FontInputsActive = false;
			bool LivePreview = true;
			bool SourceSafeMode = false;
			bool SourceCommonRuntimeInitialized = false;
			engine::imagegraph::SourceNodeInitialState SourceCommonInitialState =
				engine::imagegraph::SourceNodeInitialState::Constructed;
			uint64_t SourceCommonRuntimeRevision = 0;
			std::vector<engine::imagegraph::SourceCommonOwnerRecord> SourceCommonOwnerSnapshot;
			std::vector<SourceCollectionHostState> SourceCollections;
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
			detail::ImagePreviewPanel NodePreviews;
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
			TimelineEaseEditor EaseKeys;
			TimelineDopesheet Dopesheet;
			TimelineRegions Regions;
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
			char PxcxCollectionPath[4096] = {};
			bool PxcxCollectionPreview = false;
			std::string PxcxCollectionId;
			char PxcxAppendPath[4096] = {};
			char PxcxAppendNamespace[256] = "appended";
			char PxcxAppendContext[256] = {};
			double PxcxAppendOffset[2]{};
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
			std::string PendingGroupRender;
			std::string RouteFromEndpoint;
			std::string RouteFromPort;
			std::string RouteToEndpoint;
			std::string RouteToPort;
			std::array<char, 128> GroupName{};
			size_t ArrayPageOffset = 0;
			Document Authored;
			Document PxcxProjection;
			std::optional<engine::bake::PxcxArchive> ImportedPxcx;
			std::optional<engine::imagegraphio::PxcxCollectionSave> ImportedCollectionFiles;
			ImageGraphHistory::CollectionSnapshot CollectionManagers;
			PxcxPublishedSave PublishedPxcx;
			std::optional<PxcxPreviewIdentity> PxcxCompletedPreview;
			uint64_t PxcxPreviewCacheInputRevision = 0;
			std::vector<Diagnostic> PxcxDiagnostics;
			double ScalarPreviewValue = 0.0;
			std::vector<engine::imagegraph::AudioCaptureFrame> AudioFrames;
			std::vector<engine::imagegraph::AudioClipSource> AudioClips;
			uint64_t WavCheckerHostFrame = 0;
			Image PxcxReferenceThumbnail;
			ImageGraphPreviewCache PreviewCache;
			Diagnostic LastDiagnostic;
			ImageGraphHistory History{MAXIMUM_HISTORY};
			ImageGraphGroupHost GroupHost;
			detail::ImageGraphAxisProcessingObservers AxisObservations;
			std::string AxisSelectedNodeId;
			Status AxisReceiptFailure = Status::Ok;
			ImageGraphTimelineRead TimelineRead;
			engine::imagegraph::CapturedFeedbackHost FeedbackHost;
			detail::ImageGraphCacheEditObservation CacheEditObservation;
			detail::ImageGraphCacheGroupEdit CacheGroupEdit;
			detail::ImageGraphCacheBackgrounds CacheBackgrounds;
			detail::ImageGraphCacheEditKind CacheEditKind = detail::ImageGraphCacheEditKind::ValueSetter;
			bool CacheEditBlocked = false;
			detail::ImageGraphCacheEditKind CacheEditRetryKind = detail::ImageGraphCacheEditKind::ValueSetter;
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

		void ClearFeedbackHost(State &state) {
			state.FeedbackHost.Clear();
			state.SourceCommonRuntimeInitialized = false;
			state.SourceCommonRuntimeRevision = 0;
			state.SourceCommonOwnerSnapshot.clear();
			state.SourceCollections.clear();
		}

		State &Composer() {
			static State state;
			return state;
		}

		void RequestPreview(State &state, bool immediate = false) {
			state.PreviewDirty = true;
			if (immediate) state.NodePreviews.Request();
			state.PreviewRequested = state.PreviewRequested || immediate;
		}

		bool RetryCacheEdit(State &state) {
			if (!state.CacheEditBlocked) return true;
			state.CacheEditBlocked = !detail::ObserveImageGraphCacheEdits(
				state.Authored,
				state.CacheEditObservation,
				state.FeedbackHost,
				state.CacheEditObservation.PendingValueEdit ? state.CacheEditObservation.PendingValueKind
															: state.CacheEditRetryKind,
				state.LastDiagnostic
			);
			return !state.CacheEditBlocked;
		}

		void PublishAuthoredDocumentChanged(State &state) {
			// External authoring changes cannot retain a row selection from the old project.
			state.Playback.SelectedRegion.reset();
			state.CacheGroupEdit.Reconcile(state.Authored);
			state.PxcxCompletedPreview.reset();
			std::erase_if(state.DirectoryGrants, [&](const auto &grant) {
				return std::none_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [&](const auto &node) {
						return node.Id == grant.NodeId && node.Type == "pc.directory_search";
					}
				);
			});
			std::erase_if(state.FileGrants, [&](const auto &grant) {
				return std::none_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [&](const auto &node) {
						return node.Id == grant.NodeId &&
							   (node.Type == "pc.hlsl" || detail::ImageGraphFileReadType(node.Type) ||
								detail::ImageGraphFileWriteType(node.Type));
					}
				);
			});
			std::erase_if(state.ImageCacheLayouts, [&](const auto &observation) {
				return std::none_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [&](const auto &node) {
						return node.Id == observation.NodeId && detail::SourceImageType(node.Type);
					}
				);
			});
			std::erase_if(state.FileControls, [&](const auto &control) {
				return std::none_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [&](const auto &node) {
						return node.Id == control.NodeId &&
							   (node.Type == "pc.hlsl" || detail::ImageGraphFileReadType(node.Type) ||
								detail::ImageGraphFileWriteType(node.Type));
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
				state.PreviewSequence.Invalidate();
			} else {
				state.DocumentRevision++;
			}
			RequestPreview(state);
		}

		void AuthoredDocumentChanged(
			State &state, std::optional<detail::ImageGraphCacheEditKind> kind = std::nullopt
		) {
			if (kind == detail::ImageGraphCacheEditKind::FreshDocument) state.CacheGroupEdit.Clear();
			state.CacheEditRetryKind = kind.value_or(state.CacheEditKind);
			state.CacheEditBlocked = !detail::ObserveImageGraphCacheEdits(
				state.Authored,
				state.CacheEditObservation,
				state.FeedbackHost,
				kind.value_or(state.CacheEditKind),
				state.LastDiagnostic
			);
			PublishAuthoredDocumentChanged(state);
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
			state.Host.Directories = state.DirectoryGrants;
			state.Host.ImageCaches = state.ImageCacheLayouts;
			return state.Host;
		}

		void CancelComposerPreview(State &state, engine::render::Renderer &renderer) {
			for (const auto name : state.ComposerCaptureNames)
				renderer.CancelComposerCapture(state.ComposerCadence.Current.Owner, name);
			state.ComposerCaptureNames.clear();
			state.ComposerCadence.Cancel();
			state.PreviewSequence.Invalidate();
			state.Host.LuaReceipts.Clear();
			state.PreviewObservations.Clear();
			state.ComposerDevicePending = false;
			if (auto *host = dynamic_cast<detail::ImageGraphComposerHost *>(state.Host.Composer))
				host->Pending = host->HavePendingJobs = false;
		}
		void CancelComposerPreview(State &state) {
			if (const auto *host = dynamic_cast<detail::ImageGraphComposerHost *>(state.Host.Composer))
				CancelComposerPreview(state, host->Renderer);
		}

		bool BindObservations(
			State &state,
			engine::imagegraph::EvaluationRequest &request,
			engine::imagegraph::SourceFontContext *heldFontContext = nullptr
		) {
			if (!RetryCacheEdit(state)) return false;
			request.SourceSafeMode = state.SourceSafeMode;
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
			if (state.FontInputsActive && heldFontContext) {
				Diagnostic diagnostic;
				if (!detail::BindImageGraphFontInputs(
						*state.FontInputs,
						true,
						state.Playback.Playing,
						*heldFontContext,
						request,
						engine::imagegraph::Limits::MaximumEvaluationBytes,
						diagnostic
					)) {
					state.LastDiagnostic = std::move(diagnostic);
					return false;
				}
			}
			return true;
		}

		void ReloadCanvas(State &state);
		bool ReconcileSplitOutputs(
			State &state,
			Document &document,
			const engine::imagegraph::GroupReplayState *replay = nullptr,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) {
			std::vector<std::string> splitIds;
			for (const auto &node : document.Nodes)
				if (node.Type == "pc.array_split") splitIds.push_back(node.Id);
			if (splitIds.empty()) return true;
			engine::imagegraph::Plan plan;
			engine::imagegraph::Diagnostic error;
			const auto documentBytes = engine::imagegraph::DocumentRetainedPayloadBytes(document);
			uint64_t scratch = splitIds.capacity() * sizeof(std::string);
			for (const auto &id : splitIds)
				scratch += id.capacity();
			if (!documentBytes || *documentBytes >= maximumBytes ||
				scratch >= maximumBytes - *documentBytes) {
				state.LastDiagnostic = {
					Status::LimitExceeded, {}, {}, "split output reconciliation exceeds budget"
				};
				return false;
			}
			const auto workspace = (maximumBytes - *documentBytes - scratch) / 2;
			if (engine::imagegraph::Compile(document, plan, error, workspace) !=
				engine::imagegraph::Status::Ok) {
				if (error.Code == Status::LimitExceeded) {
					state.LastDiagnostic = error;
					return false;
				}
				return true;
			}
			engine::imagegraphphysics::RigidProvider requestRigidProvider;
			engine::imagegraph::EvaluationRequest request;
			request.HostProvider = &HostFor(state);
			request.GroupReplay = replay;
			request.GroupAuthoringRevision = replay ? replay->AuthoringRevision() : 0;
			engine::imagegraph::SourceFontContext requestFontContext;
			if (!BindObservations(state, request, &requestFontContext)) return false;
			detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
			request.AudioFrames = state.AudioFrames;
			request.AudioClips = state.AudioClips;
			(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
			std::vector<std::pair<std::string, size_t>> resized;
			for (const auto &id : splitIds) {
				engine::imagegraph::EvaluationSnapshot snapshot;
				if (engine::imagegraph::EvaluateNodeInputs(
						document, plan, id, request, snapshot, error, workspace
					) != engine::imagegraph::Status::Ok) {
					if (error.Code == Status::LimitExceeded) {
						state.LastDiagnostic = error;
						return false;
					}
					continue;
				}
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
			detail::SeedImageGraphCanvasInputs(state.Graph, state.Ids);
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
			state.Canvas.Signals.DrawBackground = [&state](const auto &graph, const auto &view) {
				(void)detail::DrawImageGraphCacheBackgrounds(
					graph,
					state.Canvas,
					view,
					state.FeedbackHost.SourceCacheGroups(),
					state.Ids.ToCanvas,
					state.CacheBackgrounds,
					state.LastDiagnostic
				);
			};
			state.Canvas.Signals.ClickNode = [&state](nodegraph::NodeId id) {
				const auto found = state.Ids.ToDocument.find(id);
				if (found == state.Ids.ToDocument.end() || state.CacheGroupEdit.OwnerId.empty()) return false;
				try {
					return state.CacheGroupEdit.QueueClick(found->second);
				} catch (const std::bad_alloc &) {
					state.LastDiagnostic = {
						Status::LimitExceeded, {}, {}, "Cache group click allocation refused"
					};
					return true;
				}
			};
			state.VectorControls.Attach(state.Canvas, state.Authored, state.Ids, state.History, [&state] {
				AuthoredDocumentChanged(state);
			});
			state.NodePreviews.Attach(state.Canvas, state.Ids);
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
			state.CacheEditRetryKind = detail::ImageGraphCacheEditKind::FreshDocument;
			state.CacheEditBlocked = !detail::ObserveImageGraphCacheEdits(
				state.Authored,
				state.CacheEditObservation,
				state.FeedbackHost,
				detail::ImageGraphCacheEditKind::FreshDocument,
				state.LastDiagnostic
			);
			ReloadCanvas(state);
		}

		constexpr uint64_t FONT_ARTIFACT_LIMIT_BYTES = 16 * 1024 * 1024;
		constexpr uint64_t FONT_ARTIFACT_PATH_SCRATCH_BYTES = 4096;
		constexpr uint64_t FONT_ARTIFACT_PUBLISH_SCRATCH_BYTES = 4096;

		std::optional<uint64_t> FontEditorHeldBytes(const State &state) {
			const auto editable =
				engine::imagegraphfont::GraphFontConfigurationRetainedBytes(state.EditableFontConfiguration);
			if (!editable) return {};
			uint64_t bytes = state.FontInputs->RetainedBytes();
			if (*editable > UINT64_MAX - bytes) return {};
			bytes += *editable;
			if (state.PendingFontConfiguration) {
				const auto pending = engine::imagegraphfont::GraphFontConfigurationRetainedBytes(
					*state.PendingFontConfiguration
				);
				if (!pending || *pending > UINT64_MAX - bytes) return {};
				bytes += *pending;
			}
			return bytes;
		}

		std::optional<uint64_t> FontEditorAvailableBytes(const State &state, uint64_t scratchBytes) {
			const auto held = FontEditorHeldBytes(state);
			const uint64_t maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (!held || *held > maximum || scratchBytes > maximum - *held) return {};
			return maximum - *held - scratchBytes;
		}

		bool AdmitEditableFontGrowth(State &state, uint64_t growthBytes) {
			const auto held = FontEditorHeldBytes(state);
			const uint64_t maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (held && *held <= maximum && growthBytes <= maximum - *held) return true;
			state.FontControls.Message = "Font editor changes exceed the retained byte budget.";
			state.LastDiagnostic = {
				Status::LimitExceeded,
				{},
				"font inputs",
				"Font editor allocation exceeds retained byte budget"
			};
			return false;
		}

		uint64_t FontStringGrowth(size_t length) {
			return uint64_t(length) + 64;
		}

		bool ReadFontArtifact(
			const std::filesystem::path &path,
			engine::imagegraphfont::GraphFontConfiguration &configuration,
			uint64_t maximumBytes,
			Diagnostic &diagnostic
		) {
			maximumBytes = std::min(maximumBytes, FONT_ARTIFACT_LIMIT_BYTES);
			std::error_code filesystemError;
			const uintmax_t size = std::filesystem::file_size(path, filesystemError);
			if (filesystemError) {
				diagnostic = {
					Status::InvalidValue,
					{},
					"font inputs",
					"Could not read font artifact size: " + filesystemError.message()
				};
				return false;
			}
			if (size > maximumBytes) {
				diagnostic = {Status::LimitExceeded, {}, "font inputs", "Font input artifact exceeds 16 MiB"};
				return false;
			}
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				diagnostic = {
					Status::InvalidValue, {}, "font inputs", "Could not open the font input artifact"
				};
				return false;
			}
			std::string bytes(static_cast<size_t>(size), '\0');
			if (!bytes.empty() && (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
								   static_cast<size_t>(file.gcount()) != bytes.size())) {
				diagnostic = {
					Status::InvalidValue, {}, "font inputs", "Could not read the font input artifact"
				};
				return false;
			}
			return engine::imagegraphfont::ReadGraphFontConfiguration(
				bytes, configuration, maximumBytes, diagnostic
			);
		}

		bool WriteFontArtifact(
			const std::filesystem::path &path,
			const engine::imagegraphfont::GraphFontConfiguration &configuration,
			uint64_t maximumBytes,
			Diagnostic &diagnostic
		) {
			maximumBytes = std::min(maximumBytes, FONT_ARTIFACT_LIMIT_BYTES);
			if (maximumBytes <= FONT_ARTIFACT_PUBLISH_SCRATCH_BYTES) {
				diagnostic = {
					Status::LimitExceeded, {}, "font inputs", "No memory remains to publish the font artifact"
				};
				return false;
			}
			std::string bytes;
			if (!engine::imagegraphfont::WriteGraphFontConfiguration(
					configuration, bytes, maximumBytes - FONT_ARTIFACT_PUBLISH_SCRATCH_BYTES, diagnostic
				))
				return false;
			return detail::PublishImageGraphFontArtifact(
				path,
				[&](const std::filesystem::path &staged) {
					std::ofstream file(staged, std::ios::binary | std::ios::trunc);
					if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
						return false;
					file.close();
					return bool(file);
				},
				diagnostic
			);
		}

		template <size_t Size>
		void
		CopyFontContextPath(std::array<char, Size> &destination, const std::optional<std::string> &source) {
			destination.fill('\0');
			if (source)
				std::memcpy(
					destination.data(), source->data(), std::min(destination.size() - 1, source->size())
				);
		}

		void DrawFontInputs(State &state) {
			auto &controls = state.FontControls;
			auto &configuration = state.EditableFontConfiguration;
			bool known = configuration.Context.AliasMapKnown;
			if (ImGui::Checkbox("Alias namespace is known", &known))
				configuration.Context.AliasMapKnown = known;
			int caseProfile = static_cast<int>(configuration.Context.TextCaseProfile);
			if (ImGui::Combo("Text case", &caseProfile, "Recorded only\0Unicode default\0"))
				configuration.Context.TextCaseProfile =
					static_cast<engine::imagegraph::FontTextCaseProfile>(caseProfile);
			int bitmapProfile = static_cast<int>(configuration.Context.BitmapTextureProfile);
			if (ImGui::Combo("Bitmap texture", &bitmapProfile, "Source observed\0Native frame UV\0"))
				configuration.Context.BitmapTextureProfile =
					static_cast<engine::imagegraph::FontBitmapTextureProfile>(bitmapProfile);
			ImGui::InputText("Default font file", controls.DefaultPath.data(), controls.DefaultPath.size());
			if (ImGui::Button("Set default path")) {
				const size_t length = std::strlen(controls.DefaultPath.data());
				const uint64_t growth = length ? FontStringGrowth(length) : 0;
				if (AdmitEditableFontGrowth(state, growth)) {
					if (length)
						configuration.Context.DefaultFontPath = controls.DefaultPath.data();
					else
						configuration.Context.DefaultFontPath.reset();
				}
			}
			ImGui::InputText("Directory", controls.Directory.data(), controls.Directory.size());
			ImGui::InputText(
				"Application location",
				controls.ApplicationLocation.data(),
				controls.ApplicationLocation.size()
			);
			ImGui::InputText("Project path", controls.ProjectPath.data(), controls.ProjectPath.size());
			if (ImGui::Button("Set context paths")) {
				const size_t directoryBytes = std::strlen(controls.Directory.data());
				const size_t applicationBytes = std::strlen(controls.ApplicationLocation.data());
				const size_t projectBytes = std::strlen(controls.ProjectPath.data());
				const uint64_t growth = (directoryBytes ? FontStringGrowth(directoryBytes) : 0) +
										(applicationBytes ? FontStringGrowth(applicationBytes) : 0) +
										(projectBytes ? FontStringGrowth(projectBytes) : 0);
				if (AdmitEditableFontGrowth(state, growth)) {
					configuration.Context.Directory =
						directoryBytes ? std::optional<std::string>(controls.Directory.data()) : std::nullopt;
					configuration.Context.ApplicationLocation =
						applicationBytes ? std::optional<std::string>(controls.ApplicationLocation.data())
										 : std::nullopt;
					configuration.Context.ProjectPath =
						projectBytes ? std::optional<std::string>(controls.ProjectPath.data()) : std::nullopt;
				}
			}
			ImGui::InputText("Alias", controls.Alias.data(), controls.Alias.size());
			ImGui::InputText("Alias path", controls.AliasPath.data(), controls.AliasPath.size());
			if (ImGui::Button("Add alias")) {
				const auto found = std::find_if(
					configuration.Context.Aliases.begin(),
					configuration.Context.Aliases.end(),
					[&](const auto &item) { return item.first == controls.Alias.data(); }
				);
				if (found == configuration.Context.Aliases.end()) {
					const uint64_t metadata = sizeof(std::pair<std::string, std::string>);
					const uint64_t growth =
						metadata + FontStringGrowth(std::strlen(controls.Alias.data())) +
						FontStringGrowth(std::strlen(controls.AliasPath.data())) +
						(configuration.Context.Aliases.size() == configuration.Context.Aliases.capacity()
							 ? 2 * (configuration.Context.Aliases.size() + 1) * metadata
							 : 0);
					if (AdmitEditableFontGrowth(state, growth))
						configuration.Context.Aliases.emplace_back(
							controls.Alias.data(), controls.AliasPath.data()
						);
				} else {
					const uint64_t growth = FontStringGrowth(std::strlen(controls.AliasPath.data()));
					if (AdmitEditableFontGrowth(state, growth)) found->second = controls.AliasPath.data();
				}
			}
			for (size_t i = 0; i < configuration.Context.Aliases.size(); ++i) {
				const auto &alias = configuration.Context.Aliases[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::Text("%s -> %s", alias.first.c_str(), alias.second.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					configuration.Context.Aliases.erase(configuration.Context.Aliases.begin() + i);
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::SeparatorText("Exact file read grants");
			ImGui::InputText("Node ID", controls.GrantNode.data(), controls.GrantNode.size());
			ImGui::InputText("Granted file", controls.GrantPath.data(), controls.GrantPath.size());
			if (ImGui::Button("Grant this file to this node")) {
				if (controls.GrantNode[0] && controls.GrantPath[0]) {
					auto &grants = configuration.ReadGrants;
					const auto found = std::find_if(grants.begin(), grants.end(), [&](const auto &grant) {
						return grant.NodeId == controls.GrantNode.data() && grant.Resource == "font";
					});
					const uint64_t growth =
						sizeof(engine::imagegraphfont::GraphFontFileGrant) +
						FontStringGrowth(std::strlen(controls.GrantNode.data())) +
						4 * FontStringGrowth(std::strlen(controls.GrantPath.data())) +
						(grants.size() == grants.capacity()
							 ? 2 * (grants.size() + 1) * sizeof(engine::imagegraphfont::GraphFontFileGrant)
							 : 0);
					if (AdmitEditableFontGrowth(state, growth)) {
						engine::imagegraphfont::GraphFontFileGrant grant{
							controls.GrantNode.data(),
							std::filesystem::path(controls.GrantPath.data()),
							false,
							"font"
						};
						if (found == grants.end())
							grants.push_back(std::move(grant));
						else
							*found = std::move(grant);
					}
				}
			}
			for (size_t i = 0; i < configuration.ReadGrants.size(); ++i) {
				const auto &grant = configuration.ReadGrants[i];
				ImGui::PushID(static_cast<int>(i + 1024));
				ImGui::Text("%s reads %s", grant.NodeId.c_str(), grant.File.string().c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("Revoke")) {
					configuration.ReadGrants.erase(configuration.ReadGrants.begin() + i);
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::SeparatorText("Owned observations");
			ImGui::Text("%zu recorded font observations", configuration.Observations.size());
			ImGui::InputText("Configuration artifact", controls.Artifact.data(), controls.Artifact.size());
			if (ImGui::Button("Load artifact into editor")) controls.PendingFontArtifactLoad = true;
			ImGui::SameLine();
			if (ImGui::Button("Save editor artifact")) controls.PendingFontArtifactSave = true;
			if (ImGui::Button("Apply font inputs")) controls.PendingApplyFontInputs = true;
			ImGui::SameLine();
			if (ImGui::Button("Clear observations")) controls.PendingClearFontObservations = true;
			if (!controls.Message.empty()) ImGui::TextWrapped("%s", controls.Message.c_str());
		}

		void CancelExportIntent(State &state, engine::render::Renderer &renderer);

		void ApplyPendingFontInputs(State &state, engine::render::Renderer &renderer) {
			auto &controls = state.FontControls;
			if (controls.PendingFontArtifactLoad) {
				controls.PendingFontArtifactLoad = false;
				engine::imagegraphfont::GraphFontConfiguration candidate;
				Diagnostic diagnostic;
				const auto available = FontEditorAvailableBytes(state, FONT_ARTIFACT_PATH_SCRATCH_BYTES);
				if (available &&
					ReadFontArtifact(controls.Artifact.data(), candidate, *available, diagnostic)) {
					state.EditableFontConfiguration = std::move(candidate);
					CopyFontContextPath(
						controls.DefaultPath, state.EditableFontConfiguration.Context.DefaultFontPath
					);
					CopyFontContextPath(
						controls.Directory, state.EditableFontConfiguration.Context.Directory
					);
					CopyFontContextPath(
						controls.ApplicationLocation,
						state.EditableFontConfiguration.Context.ApplicationLocation
					);
					CopyFontContextPath(
						controls.ProjectPath, state.EditableFontConfiguration.Context.ProjectPath
					);
					controls.Message = "Artifact loaded. Apply font inputs to use this configuration.";
				} else {
					if (!available)
						diagnostic = {
							Status::LimitExceeded, {}, "font inputs", "No memory remains to load the artifact"
						};
					controls.Message = diagnostic.Message;
					state.LastDiagnostic = std::move(diagnostic);
				}
			}
			if (controls.PendingFontArtifactSave) {
				controls.PendingFontArtifactSave = false;
				Diagnostic diagnostic;
				const auto available = FontEditorAvailableBytes(state, FONT_ARTIFACT_PATH_SCRATCH_BYTES);
				const bool written =
					available &&
					WriteFontArtifact(
						controls.Artifact.data(), state.EditableFontConfiguration, *available, diagnostic
					);
				if (!available)
					diagnostic = {
						Status::LimitExceeded, {}, "font inputs", "No memory remains to save the artifact"
					};
				controls.Message = written ? "Font input artifact saved." : diagnostic.Message;
				if (!written) state.LastDiagnostic = std::move(diagnostic);
			}
			if (controls.PendingClearFontObservations) {
				controls.PendingClearFontObservations = false;
				state.EditableFontConfiguration.Observations.clear();
			}
			if (controls.PendingApplyFontInputs) {
				controls.PendingApplyFontInputs = false;
				const auto editableBytes = engine::imagegraphfont::GraphFontConfigurationRetainedBytes(
					state.EditableFontConfiguration
				);
				const uint64_t ownerBytes = state.FontInputs->RetainedBytes();
				const uint64_t maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
				if (!editableBytes || ownerBytes > maximum || *editableBytes > (maximum - ownerBytes) / 2) {
					state.FontControls.Message =
						"Editable and pending font configurations exceed the byte budget.";
					state.LastDiagnostic = {
						Status::LimitExceeded,
						{},
						"font inputs",
						"Pending font configuration clone exceeds its byte budget"
					};
					return;
				}
				try {
					state.PendingFontConfiguration = state.EditableFontConfiguration;
				} catch (const std::bad_alloc &) {
					state.FontControls.Message = "Could not allocate pending font configuration.";
					state.LastDiagnostic = {
						Status::LimitExceeded,
						{},
						"font inputs",
						"Pending font configuration allocation was refused"
					};
					return;
				}
			}
			if (!state.PendingFontConfiguration) return;
			Diagnostic diagnostic;
			const auto pendingBytes =
				engine::imagegraphfont::GraphFontConfigurationRetainedBytes(*state.PendingFontConfiguration);
			const auto editableBytes =
				engine::imagegraphfont::GraphFontConfigurationRetainedBytes(state.EditableFontConfiguration);
			const uint64_t ownerBytes = state.FontInputs->RetainedBytes();
			const uint64_t maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (!pendingBytes || !editableBytes || ownerBytes > maximum ||
				*editableBytes > maximum - ownerBytes ||
				*pendingBytes > maximum - ownerBytes - *editableBytes) {
				diagnostic = {
					Status::LimitExceeded,
					{},
					"font inputs",
					"Font owner, editor and pending copies exceed their byte budget"
				};
				state.FontControls.Message = diagnostic.Message;
				state.LastDiagnostic = std::move(diagnostic);
				state.PendingFontConfiguration.reset();
				return;
			}
			if (!state.FontInputs->Replace(
					*state.PendingFontConfiguration,
					engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
					maximum - *editableBytes,
					diagnostic
				)) {
				state.FontControls.Message = diagnostic.Message;
				state.LastDiagnostic = std::move(diagnostic);
				state.PendingFontConfiguration.reset();
				return;
			}
			state.EditableFontConfiguration = std::move(*state.PendingFontConfiguration);
			state.PendingFontConfiguration.reset();
			state.FontInputsActive = true;
			state.FontControls.Message.clear();
			CancelExportIntent(state, renderer);
			CancelComposerPreview(state, renderer);
			ClearFeedbackHost(state);
			state.PreviewCache.Clear();
			state.PxcxCompletedPreview.reset();
			if (++state.EvaluationInputRevision == 0) state.EvaluationInputRevision = 1;
			RequestPreview(state, true);
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

		void CancelExportIntent(State &state, engine::render::Renderer &renderer) {
			if (state.ExportIntent.Current && state.ExportIntent.Current->Automatic &&
				state.ExportIntent.Current->Event == detail::ImageGraphExportEvent::Update)
				state.ComposerExports.CompleteFront(state.ExportIntent.Current->Observation);
			if (state.ExportIntent.Current)
				for (const auto name : state.ExportIntent.CaptureNames)
					renderer.CancelComposerCapture(state.ExportIntent.Current->Owner, name);
			state.RangeExport.reset();
			state.ExportIntent.Clear();
		}

		bool ExportGrantsMatch(State &state) {
			const auto &intent = state.ExportIntent;
			if (!intent.Current) return false;
			if (intent.Current->Operation == detail::ImageGraphExportIntent::Kind::HostNode) return true;
			for (const auto &target : intent.Current->Targets) {
				const auto grant =
					std::find_if(state.ExportGrants.begin(), state.ExportGrants.end(), [&](const auto &item) {
						return item.NodeId == target.NodeId;
					});
				if (grant == state.ExportGrants.end() || grant->Root.string() != target.Root ||
					std::string_view(grant->ImageEncoder.data()) != target.ImageEncoder ||
					std::string_view(grant->VideoEncoder.data()) != target.VideoEncoder)
					return false;
			}
			return true;
		}

		engine::imagegraphexport::GraphExportGeneration ExportGeneration(const State &state) {
			const auto &batch = *state.ExportIntent.Current;
			return {
				batch.Observation.Revision,
				batch.Observation.InputRevision,
				state.ExportIntentGeneration,
				state.ExportIntentGeneration,
				batch.Owner.Id()
			};
		}

		void ResumeExportIntent(State &state) {
			if (!RetryCacheEdit(state)) return;
			auto *composer = dynamic_cast<detail::ImageGraphComposerHost *>(state.Host.Composer);
			auto &intent = state.ExportIntent;
			if (!composer || !intent.Current) return;
			if (!intent.Matches(composer->Owner, state.DocumentRevision, state.EvaluationInputRevision) ||
				!ExportGrantsMatch(state)) {
				state.LastDiagnostic = {
					Status::InvalidValue,
					{},
					"export",
					"Export canceled because its owner, project, inputs or grants changed"
				};
				CancelExportIntent(state, composer->Renderer);
				return;
			}
			engine::imagegraph::Plan plan;
			Diagnostic error;
			const auto compiled = engine::imagegraph::Compile(state.Authored, plan, error);
			const detail::ImageGraphComposerExportScope captureScope(
				state.Host.Composer, intent.CaptureNames
			);
			intent.Observations.BeginAttempt();
			while (const auto *target = intent.Selected()) {
				engine::imagegraphphysics::RigidProvider rigid;
				engine::imagegraph::EvaluationRequest request;
				const auto &batch = *intent.Current;
				batch.Observation.Pcx.Bind(request);
				request.SourceSafeMode = state.SourceSafeMode;
				detail::BindImageGraphRigid(request, rigid, batch.Observation.Playback);
				std::unique_ptr<engine::imagegraphfont::GraphFontInputs> frozenFonts;
				engine::imagegraph::SourceFontContext heldFontContext;
				uint64_t evaluationAllowance = engine::imagegraph::Limits::MaximumEvaluationBytes;
				if (batch.FontConfiguration) {
					const auto frozenBytes =
						engine::imagegraphfont::GraphFontConfigurationRetainedBytes(*batch.FontConfiguration);
					if (!frozenBytes || *frozenBytes > evaluationAllowance) {
						state.LastDiagnostic = {
							Status::LimitExceeded,
							target->NodeId,
							"font inputs",
							"Frozen font configuration exceeds its byte budget"
						};
						CancelExportIntent(state, composer->Renderer);
						return;
					}
					evaluationAllowance -= *frozenBytes;
					frozenFonts = std::make_unique<engine::imagegraphfont::GraphFontInputs>();
					if (!frozenFonts->Replace(
							*batch.FontConfiguration,
							engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
							evaluationAllowance,
							error
						)) {
						state.LastDiagnostic = std::move(error);
						CancelExportIntent(state, composer->Renderer);
						return;
					}
					if (!detail::BindImageGraphFontInputs(
							*frozenFonts,
							true,
							batch.Observation.Playback.Playing,
							heldFontContext,
							request,
							evaluationAllowance,
							error
						)) {
						state.LastDiagnostic = std::move(error);
						CancelExportIntent(state, composer->Renderer);
						return;
					}
					if (*frozenBytes > UINT64_MAX - request.SourceFontHostResidentBytes) {
						state.LastDiagnostic = {
							Status::LimitExceeded,
							target->NodeId,
							"font inputs",
							"Frozen font residency exceeds its byte limit"
						};
						CancelExportIntent(state, composer->Renderer);
						return;
					}
					request.SourceFontHostResidentBytes += *frozenBytes;
				}
				if (!state.RangeExport && !intent.Preparation.Current && compiled == Status::Ok) {
					std::vector<std::string> outputs;
					bool feedback = false;
					if (!state.SelectedOutput.empty()) outputs.push_back(state.SelectedOutput);
					for (const auto &node : state.Authored.Nodes)
						if (node.Type == "image.captured")
							for (const auto &input : node.Values)
								if (input.Port == "source_id")
									if (const auto *source = std::get_if<std::string>(&input.Data);
										source && source->starts_with("feedback:")) {
										const auto output = std::find_if(
											state.Authored.Outputs.begin(),
											state.Authored.Outputs.end(),
											[&](const auto &item) {
												return item.Id == std::string_view(*source).substr(9);
											}
										);
										if (output != state.Authored.Outputs.end() &&
											std::find(outputs.begin(), outputs.end(), output->Id) ==
												outputs.end())
											outputs.push_back(output->Id);
										feedback = true;
									}
					const auto cone = engine::imagegraph::AnalyzeStatefulTemporalCone(
						state.Authored, plan, outputs, target->NodeId
					);
					const bool direct = cone.DataProcessors && !cone.FirstFrameData && !cone.Simulation &&
										!cone.SurfaceCaches && !cone.RandomGenerators && !cone.RigidActors &&
										!feedback;
					const bool replay =
						cone.Valid && !direct &&
						(feedback || state.FeedbackHost.Active() || cone.Simulation || cone.SurfaceCaches ||
						 cone.RandomGenerators || cone.RigidActors || cone.FirstFrameData);
					std::string admission;
					const auto final = GetImageGraphFrame(batch.Observation.Playback);
					if (!intent.Preparation.Begin(
							state.FeedbackHost.PreparedFrame(
								state.DocumentRevision, state.EvaluationInputRevision
							),
							final,
							replay && !((cone.FixedSimulationSteps || cone.SurfaceCaches || feedback) &&
										final.Subframe != 0),
							admission
						)) {
						state.LastDiagnostic = {
							Status::LimitExceeded, target->NodeId, "export", std::move(admission)
						};
						CancelExportIntent(state, composer->Renderer);
						return;
					}
				}
				const auto frame =
					state.RangeExport ? state.RangeExport->NextFrame() : intent.Preparation.Current;
				if (frame) (void)engine::imagegraph::SetFrameTime(request, *frame);
				// The matching generation above precedes every read of these caller-owned
				// spans.
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				if (batch.Operation == detail::ImageGraphExportIntent::Kind::HostNode)
					request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
				detail::ImageGraphExportProvider provider(HostFor(state), intent, request);
				provider.CaptureReceipts = !state.RangeExport;
				provider.PendingFlag = &composer->Pending;
				provider.CancelContext = &composer->Renderer;
				provider.CancelNames = [](void *context,
										  engine::core::Name owner,
										  std::span<const engine::core::Name> names) noexcept {
					auto &renderer = *static_cast<engine::render::Renderer *>(context);
					for (const auto name : names)
						renderer.CancelComposerCapture(owner, name);
				};
				request.HostProvider = &provider;
				composer->Pending = false;
				std::string failure;
				bool complete = false, failed = false, skipped = false;
				if (state.RangeExport) {
					const auto progress =
						state.RangeExport->Resume(request, ExportGeneration(state), provider, failure);
					if (progress == engine::imagegraphexport::GraphExportProgress::Pending ||
						progress == engine::imagegraphexport::GraphExportProgress::Progress) {
						intent.Pending = true;
						auto &grant = GrantFor(state, target->NodeId);
						grant.Failed = false;
						grant.Message = progress == engine::imagegraphexport::GraphExportProgress::Pending
											? "Export waiting for surface capture."
											: "Export collecting frames.";
						return;
					}
					complete = progress == engine::imagegraphexport::GraphExportProgress::Complete;
					failed = !complete;
					auto &grant = GrantFor(state, target->NodeId);
					const auto directories = state.RangeExport->RetainedDirectories();
					grant.RetainedFrames.assign(directories.begin(), directories.end());
				} else {
					engine::imagegraph::EvaluationSnapshot directInputs;
					const engine::imagegraph::EvaluationSnapshot *snapshot = nullptr;
					if (compiled != Status::Ok)
						failure = error.Message;
					else if (!state.GroupHost.Prepare(
								 state.Authored, plan, state.DocumentRevision, request, error
							 ) ||
							 !state.FeedbackHost.Prepare(
								 state.Authored,
								 plan,
								 state.DocumentRevision,
								 state.EvaluationInputRevision,
								 request,
								 error,
								 evaluationAllowance,
								 state.SelectedOutput,
								 target->NodeId
							 ))
						failure = error.Message;
					else if (state.FeedbackHost.Active())
						snapshot = &state.FeedbackHost.Snapshot();
					else if (engine::imagegraph::EvaluateNodeInputs(
								 state.Authored,
								 plan,
								 target->NodeId,
								 request,
								 directInputs,
								 error,
								 evaluationAllowance
							 ) == Status::Ok)
						snapshot = &directInputs;
					else
						failure = error.Message;
					if (snapshot && !intent.Preparation.Complete()) {
						intent.Observations.Clear();
						intent.Pending = true;
						return;
					}
					if (!snapshot) {
						if (provider.Pending()) {
							intent.Finish(detail::ImageGraphExportIntent::Result::Pending);
							if (batch.Operation == detail::ImageGraphExportIntent::Kind::Authored) {
								auto &grant = GrantFor(state, target->NodeId);
								grant.Message = "Export waiting for surface capture.";
								grant.Failed = false;
							} else {
								const auto control = std::find_if(
									state.FileControls.begin(),
									state.FileControls.end(),
									[&](const auto &item) { return item.NodeId == target->NodeId; }
								);
								if (control != state.FileControls.end())
									control->Message = "File action waiting for surface capture.";
							}
							return;
						}
						failed = true;
					} else if (batch.Operation == detail::ImageGraphExportIntent::Kind::HostNode) {
						engine::imagegraph::HostNodeCapture captured;
						bool imageChanged = false;
						if (target->ImageAction) {
							const auto documentBytes =
								engine::imagegraph::DocumentRetainedPayloadBytes(state.Authored);
							const uint64_t replayBytes = state.GroupHost.Replay.RetainedBytes();
							const uint64_t limit = engine::imagegraph::Limits::MaximumEvaluationBytes;
							const uint64_t maximum =
								limit - std::min(request.SourceFontHostResidentBytes, limit);
							complete = documentBytes && *documentBytes < maximum &&
									   replayBytes < maximum - *documentBytes;
							if (!complete)
								failure = "Held authoring owners leave no image action budget";
							else
								complete = detail::PrepareImageGraphImageControls(
									state.Authored,
									request,
									target->NodeId,
									*snapshot,
									captured,
									error,
									maximum - *documentBytes - replayBytes
								);
							if (!complete && failure.empty()) failure = error.Message;
							engine::imagegraphio::SourceImageFrameObservation prepared;
							if (complete && *target->ImageAction ==
												engine::imagegraphio::SourceImageAction::RemoveCache) {
								prepared.Controls = std::move(captured);
								prepared.Kind = engine::imagegraphio::SourceImageFrameKind::ControlsOnly;
								prepared.AuthoringRevision = state.DocumentRevision;
								prepared.InputRevision = state.EvaluationInputRevision;
							} else if (complete) {
								complete = engine::imagegraphexport::PrepareGraphSourceImages(
									captured,
									state.FileGrants,
									engine::assets::ContentPolicy::Process(
										engine::assets::ContentVerb::Handle
									),
									state.DocumentRevision,
									state.EvaluationInputRevision,
									*target->ImageAction == engine::imagegraphio::SourceImageAction::Cache,
									prepared,
									failure,
									maximum - *documentBytes - replayBytes - snapshot->RetainedBytes()
								);
							}
							if (complete) {
								// The borrowed snapshot and temporary receipt stay resident through
								// authoring.
								const auto receiptBytes =
									engine::imagegraph::HostCaptureRetainedPayloadBytes(captured);
								const uint64_t nextRevision =
									state.DocumentRevision == UINT64_MAX ? 1 : state.DocumentRevision + 1;
								complete = receiptBytes && snapshot->RetainedBytes() <= maximum &&
										   *receiptBytes <= maximum - snapshot->RetainedBytes();
								if (complete)
									complete = detail::ApplyPreparedImageGraphImage(
										state.Authored,
										state.History,
										state.GroupHost.Replay,
										prepared,
										{*target->ImageAction,
										 state.DocumentRevision,
										 nextRevision,
										 state.EvaluationInputRevision},
										error,
										imageChanged,
										maximum - snapshot->RetainedBytes() - *receiptBytes
									);
								if (!complete)
									failure = error.Message.empty()
												  ? "Prepared image action exceeds operation bounds"
												  : error.Message;
							}
						} else
							complete = engine::imagegraphexport::ExecuteGraphHostNode(
								state.Authored, request, target->NodeId, *snapshot, captured, failure
							);

						if (complete && target->ArtworkAction) {
							bool changed = false;
							const uint64_t nextRevision =
								state.DocumentRevision == UINT64_MAX ? 1 : state.DocumentRevision + 1;
							const engine::imagegraphio::SourceArtworkEditOptions options{
								*target->ArtworkAction,
								target->MatchRegionNames,
								true,
								state.DocumentRevision,
								nextRevision,
								std::nullopt
							};
							complete = detail::ApplyPreparedImageGraphArtwork(
								state.Authored,
								state.History,
								state.GroupHost.Replay,
								captured,
								options,
								error,
								changed
							);
							if (!complete) failure = error.Message;
							if (changed) {
								const auto frame = GetImageGraphFrame(state.Playback);
								ApplyImageGraphTimeline(state.Authored, state.Playback);
								(void)SetImageGraphAuthorFrame(state.Playback, frame);
								AuthoredDocumentChanged(state);
								state.GroupHost.Revision = state.DocumentRevision;
								state.CanvasNeedsReload = true;
							}
						}
						if (imageChanged) {
							const auto frame = GetImageGraphFrame(state.Playback);
							ApplyImageGraphTimeline(state.Authored, state.Playback);
							(void)SetImageGraphAuthorFrame(state.Playback, frame);
							AuthoredDocumentChanged(state);
							state.GroupHost.Revision = state.DocumentRevision;
							state.CanvasNeedsReload = true;
						}
						failed = !complete;
					} else if (batch.Automatic &&
							   !detail::SourceExportTriggered(snapshot->Values(), batch.Event)) {
						complete = true;
						skipped = true;
					} else if (target->Root.empty()) {
						failure = "Grant an export directory before running this node.";
						failed = true;
					} else {
						engine::imagegraphexport::GraphExportSettings settings;
						settings.Input = target->ProjectPath;
						settings.Output = target->Root;
						settings.ImageEncoder = target->ImageEncoder;
						settings.VideoEncoder = target->VideoEncoder;
						settings.NativeGif = true;
						const auto type = std::find_if(
							snapshot->Values().begin(), snapshot->Values().end(), [](const auto &input) {
								return input.Port == "type";
							}
						);
						bool animation = false;
						if (type != snapshot->Values().end()) {
							if (const auto *choice = std::get_if<engine::imagegraph::EnumValue>(&type->Data))
								animation = choice->Value != 0;
							else if (const auto *integer = std::get_if<int64_t>(&type->Data))
								animation = *integer != 0;
							else if (const auto *scalar = std::get_if<double>(&type->Data))
								animation = *scalar != 0;
						}
						if (animation) {
							const auto documentBytes =
								engine::imagegraph::DocumentRetainedPayloadBytes(state.Authored);
							const auto metadataBytes = intent.MetadataBytes();
							const auto observationBytes = intent.Observations.RetainedPayloadBytes();
							const auto frozenConfigurationBytes =
								batch.FontConfiguration
									? engine::imagegraphfont::GraphFontConfigurationRetainedBytes(
										  *batch.FontConfiguration
									  )
									: std::optional<uint64_t>{0};
							const auto rangeMetadataBytes =
								metadataBytes && frozenConfigurationBytes &&
										*frozenConfigurationBytes <= *metadataBytes
									? std::optional<uint64_t>{*metadataBytes - *frozenConfigurationBytes}
									: std::optional<uint64_t>{};
							const std::array<uint64_t, 8> held{
								documentBytes.value_or(UINT64_MAX),
								rangeMetadataBytes.value_or(UINT64_MAX),
								observationBytes.value_or(UINT64_MAX),
								snapshot->RetainedBytes(),
								state.Host.RetainedObservationBytes(),
								state.Host.LuaReceipts.Bytes,
								request.SourceFontHostResidentBytes,
								engine::imagegraph::Limits::MaximumEvaluationBytes
							};
							const bool admitted = documentBytes && rangeMetadataBytes && observationBytes &&
												  detail::ImageGraphExportIntent::AdmitRangePayload(
													  held,
													  engine::imagegraphexport::GraphExportSession::
														  MaximumPayloadReservationBytes(),
													  failure
												  );
							if (!admitted && failure.empty())
								failure = "Range export retained inputs cannot be accounted";
							auto session =
								admitted ? std::make_unique<engine::imagegraphexport::GraphExportSession>()
										 : nullptr;
							if (session && session->BeginAuthored(
											   state.Authored,
											   *snapshot,
											   request,
											   settings,
											   target->NodeId,
											   ExportGeneration(state),
											   failure
										   )) {
								state.RangeExport = std::move(session);
								intent.Observations.Clear();
								intent.Pending = true;
								auto &grant = GrantFor(state, target->NodeId);
								grant.Message = "Export collecting frames.";
								grant.Failed = false;
								return;
							}
							failed = true;
						} else {
							auto &grant = GrantFor(state, target->NodeId);
							std::vector<std::filesystem::path> retained;
							complete = engine::imagegraphexport::ExportAuthoredGraphNode(
								state.Authored,
								plan,
								request,
								settings,
								target->NodeId,
								*snapshot,
								failure,
								&retained
							);
							failed = !complete;
							grant.RetainedFrames = std::move(retained);
						}
					}
				}
				if (intent.Current->Operation == detail::ImageGraphExportIntent::Kind::Authored) {
					auto &grant = GrantFor(state, target->NodeId);
					grant.Message = skipped ? std::string{} : (complete ? "Export complete." : failure);
					grant.Failed = failed;
				} else {
					const auto control = std::find_if(
						state.FileControls.begin(), state.FileControls.end(), [&](const auto &item) {
							return item.NodeId == target->NodeId;
						}
					);
					if (control != state.FileControls.end())
						control->Message = complete ? "File action complete." : failure;
				}
				if (state.RangeExport) {
					state.RangeExport->Cancel(provider);
					state.RangeExport.reset();
				}
				if (intent.Finish(
						failed ? detail::ImageGraphExportIntent::Result::Failed
							   : detail::ImageGraphExportIntent::Result::Complete
					)) {
					CancelExportIntent(state, composer->Renderer);
					RequestPreview(state, true);
					return;
				}
			}
		}

		bool BeginExportIntent(
			State &state,
			detail::ImageGraphExportIntent::Kind kind,
			std::span<const detail::ImageGraphExportIntent::Target> targets,
			bool automatic,
			detail::ImageGraphExportEvent event,
			const detail::ImageGraphComposerExports::Observation *recorded = nullptr
		) {
			auto *composer = dynamic_cast<detail::ImageGraphComposerHost *>(state.Host.Composer);
			if (!composer) return false;
			engine::imagegraph::EvaluationRequest request;
			if (!recorded) BindObservations(state, request);
			const auto observation = recorded ? *recorded
											  : detail::ImageGraphComposerExports::Observation{
													state.Playback,
													state.PcxObservations,
													state.DocumentRevision,
													state.EvaluationInputRevision
												};
			std::optional<engine::imagegraphfont::GraphFontConfiguration> frozenFontConfiguration;
			if (state.FontInputsActive) {
				const auto editableBytes = engine::imagegraphfont::GraphFontConfigurationRetainedBytes(
					state.EditableFontConfiguration
				);
				const uint64_t ownerBytes = state.FontInputs->RetainedBytes();
				if (!editableBytes || ownerBytes > engine::imagegraph::Limits::MaximumEvaluationBytes ||
					*editableBytes > engine::imagegraph::Limits::MaximumEvaluationBytes - ownerBytes) {
					state.LastDiagnostic = {
						Status::LimitExceeded,
						{},
						"font inputs",
						"Held Studio font configuration exceeds its byte budget"
					};
					return false;
				}
				const uint64_t heldFontBytes = ownerBytes + *editableBytes;
				uint64_t frozenBudget = engine::imagegraph::Limits::MaximumEvaluationBytes - heldFontBytes;
				const bool hasTextNodes = std::any_of(
					state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [](const auto &node) {
						return node.Type == "pc.text";
					}
				);
				const bool freezeTextFonts = observation.Playback.Playing && hasTextNodes;
				if (freezeTextFonts) {
					const auto frame = GetImageGraphFrame(observation.Playback);
					if (state.PreviewDirty || state.PreviewRequested || !state.PxcxCompletedPreview ||
						state.PxcxCompletedPreview->DocumentRevision != observation.Revision ||
						state.PxcxCompletedPreview->InputRevision != observation.InputRevision ||
						state.PxcxCompletedPreview->Frame != frame) {
						state.LastDiagnostic = {
							Status::InvalidValue,
							{},
							"font inputs",
							"Text font seeds require a completed preview for the held export frame"
						};
						return false;
					}
					const auto preparedFrame =
						state.FeedbackHost.PreparedFrame(observation.Revision, observation.InputRevision);
					const auto *preparedData =
						state.FeedbackHost.PreparedData(observation.Revision, observation.InputRevision);
					if (!preparedFrame || preparedFrame->Tick != frame.Tick ||
						preparedFrame->Subframe != frame.Subframe ||
						preparedFrame->NegativeFrame != frame.NegativeFrame || !preparedData) {
						state.LastDiagnostic = {
							Status::InvalidValue,
							{},
							"font inputs",
							"Matching prepared Text font replay is unavailable"
						};
						return false;
					}
					const uint64_t replayBytes = engine::imagegraph::RetainedDataReplayBytes(*preparedData);
					if (*editableBytes > frozenBudget || replayBytes > frozenBudget - *editableBytes ||
						engine::imagegraph::ValidateDataReplay(
							*preparedData, frozenBudget - *editableBytes, state.LastDiagnostic
						) != Status::Ok) {
						if (state.LastDiagnostic.Code == Status::Ok)
							state.LastDiagnostic = {
								Status::LimitExceeded,
								{},
								"font inputs",
								"Held replay and cloned font configuration exceed their byte budget"
							};
						return false;
					}
				}
				if (*editableBytes > frozenBudget) {
					state.LastDiagnostic = {
						Status::LimitExceeded,
						{},
						"font inputs",
						"Held and frozen Studio font configurations exceed their byte budget"
					};
					return false;
				}
				frozenFontConfiguration = state.EditableFontConfiguration;
				if (freezeTextFonts) {
					const auto frame = GetImageGraphFrame(observation.Playback);
					if (!detail::FreezePreparedImageGraphInitialTextFonts(
							state.Authored,
							state.FeedbackHost,
							observation.Revision,
							observation.InputRevision,
							frame,
							*frozenFontConfiguration,
							frozenBudget,
							state.LastDiagnostic
						))
						return false;
				}
				if (!detail::ImageGraphFontConfigurationFitsBudget(
						*frozenFontConfiguration,
						heldFontBytes,
						engine::imagegraph::Limits::MaximumEvaluationBytes,
						state.LastDiagnostic
					))
					return false;
			}
			if (state.ExportIntent.Current) CancelExportIntent(state, composer->Renderer);
			if (!automatic) CancelComposerPreview(state, composer->Renderer);
			std::string failure;
			if (!state.ExportIntent.Begin(
					kind,
					composer->Owner,
					observation,
					targets,
					automatic,
					failure,
					detail::ImageGraphExportIntent::MaximumBytes,
					event,
					frozenFontConfiguration ? &*frozenFontConfiguration : nullptr
				)) {
				state.LastDiagnostic = {Status::LimitExceeded, {}, "export", std::move(failure)};
				return false;
			}
			if (++state.ExportIntentGeneration == 0) state.ExportIntentGeneration = 1;
			ResumeExportIntent(state);
			return true;
		}

		void RunAuthoredExports(
			State &state,
			detail::ImageGraphExportEvent event,
			std::string_view explicitNode = {},
			const detail::ImageGraphComposerExports::Observation *recorded = nullptr
		) {
			const uint64_t sequence = recorded ? recorded->Sequence : 0;
			const uint64_t revision = recorded ? recorded->Revision : 0;
			const uint64_t inputRevision = recorded ? recorded->InputRevision : 0;
			const auto terminal = [&] {
				if (sequence != 0 && explicitNode.empty() && event == detail::ImageGraphExportEvent::Update)
					state.ComposerExports.CompleteFront(sequence, revision, inputRevision);
			};
			std::vector<detail::ImageGraphExportIntent::Target> targets;
			for (const auto &node : state.Authored.Nodes) {
				if (node.Type != "pc.export" || (!explicitNode.empty() && node.Id != explicitNode)) continue;
				if (targets.size() == 64) {
					state.LastDiagnostic = {
						Status::LimitExceeded, {}, "export", "Export target count exceeds its session limit"
					};
					terminal();
					return;
				}
				const auto &grant = GrantFor(state, node.Id);
				targets.push_back(
					{node.Id,
					 grant.Root.string(),
					 grant.ImageEncoder.data(),
					 grant.VideoEncoder.data(),
					 (state.PxcxPathDisplay.empty()
						  ? std::filesystem::path(state.GraphName).replace_extension(".graph")
						  : std::filesystem::path(state.PxcxPathDisplay))
						 .string()}
				);
			}
			if (targets.empty()) {
				terminal();
				return;
			}
			if (!BeginExportIntent(
					state,
					detail::ImageGraphExportIntent::Kind::Authored,
					targets,
					explicitNode.empty(),
					event,
					recorded
				))
				terminal();
		}

		// core charges document and replay clones; studio charges the owners beside them.
		// reserve the source-cache loader's separate 16 MiB allowance without borrowing its owner.
		std::optional<uint64_t> GroupConstructorAllowance(const State &state, uint64_t scratchBytes = 0) {
			using namespace engine::imagegraph;
			uint64_t remaining = Limits::MaximumEvaluationBytes;
			const auto charge = [&](std::optional<uint64_t> bytes) {
				if (!bytes || *bytes >= remaining) return false;
				remaining -= *bytes;
				return true;
			};
			const auto keyAllowance = state.Keys.Remaining(true, true, Limits::MaximumEvaluationBytes, false);
			if (!keyAllowance || !charge(Limits::MaximumEvaluationBytes - *keyAllowance) ||
				!charge(state.Dopesheet.CacheBytes()) || !charge(state.EaseKeys.OriginalAxes.capacity()) ||
				!charge(
					(state.EaseKeys.Originals.capacity() - state.EaseKeys.Originals.size()) * sizeof(Keyframe)
				))
				return {};
			for (const auto &key : state.EaseKeys.Originals)
				if (!charge(KeyframePayloadBytes(key))) return {};
			if (state.ImportedCollectionFiles &&
				(!charge(state.ImportedCollectionFiles->GraphJson.capacity() + 1) ||
				 (state.ImportedCollectionFiles->MetadataJson &&
				  !charge(state.ImportedCollectionFiles->MetadataJson->capacity() + 1))))
				return {};
			if (!charge(scratchBytes) || !charge(state.PreviewSequence.RetainedBytes()) ||
				!charge(state.PreviewCache.RetainedBytes()) ||
				!charge(ValueClonePayloadBytes(state.ValuePreview)) ||
				!charge(ValueClonePayloadBytes(state.ArrayPreview)) ||
				!charge(state.FeedbackHost.RetainedBytes()) || !charge(FontEditorHeldBytes(state)) ||
				!charge(state.TimelineRead.RetainedBytes()) ||
				!charge(state.Host.RetainedObservationBytes()) || !charge(state.Host.LuaReceipts.Bytes) ||
				!charge(state.PreviewObservations.Receipts.RetainedPayloadBytes()) ||
				!charge(Limits::MaximumEvaluationBytes / 8))
				return {};
			const auto chargeVector = [&](const auto &values) {
				using Item = typename std::decay_t<decltype(values)>::value_type;
				if (values.capacity() > remaining / sizeof(Item)) return false;
				return charge(values.capacity() * sizeof(Item));
			};
			const auto chargeAudio = [&](const auto &audio) {
				if (audio.Channels.size() > Limits::MaximumAudioChannels || !chargeVector(audio.Samples) ||
					!chargeVector(audio.Channels))
					return false;
				for (const auto &channel : audio.Channels)
					if (!chargeVector(channel)) return false;
				return true;
			};
			if (state.AudioFrames.size() > Limits::MaximumAudioCaptureFrames ||
				state.AudioClips.size() > Limits::MaximumNodes || !chargeVector(state.AudioFrames) ||
				!chargeVector(state.AudioClips))
				return {};
			for (const auto &frame : state.AudioFrames)
				if (!charge(frame.SourceId.capacity()) || !charge(1) || !chargeAudio(frame)) return {};
			for (const auto &clip : state.AudioClips)
				if (!charge(clip.SourceId.capacity()) || !charge(1) || !chargeAudio(clip.Data)) return {};
			return remaining;
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
			Document projected;
			const Document *saved = &state.Authored;
			if (state.GroupHost.Revision == state.DocumentRevision &&
				state.GroupHost.Replay.InstancesBound()) {
				const auto allowance = GroupConstructorAllowance(state);
				if (!allowance) {
					state.LastDiagnostic = {
						Status::LimitExceeded, {}, {}, "retained Studio owners leave no source save allowance"
					};
					state.GraphIoMessage = state.LastDiagnostic.Message;
					return false;
				}
				if (!state.GroupHost.ProjectForSave(
						state.Authored, state.DocumentRevision, projected, state.LastDiagnostic, *allowance
					)) {
					state.GraphIoMessage = state.LastDiagnostic.Message;
					return false;
				}
				saved = &projected;
			}
			const std::string text = engine::imagegraph::Write(*saved);
			if (text.empty()) {
				state.GraphIoMessage = "native graph has no valid save projection";
				return false;
			}
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
			if (!detail::PublishImageGraphFile(
					path,
					std::as_bytes(std::span(text.data(), text.size())),
					state.LastDiagnostic,
					IMAGE_GRAPH_FILE_MAXIMUM_BYTES
				)) {
				state.GraphIoMessage = state.LastDiagnostic.Message;
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
			ClearFeedbackHost(state);
			state.SourceCommonInitialState = engine::imagegraph::SourceNodeInitialState::Loaded;
			state.ExportGrants.clear();
			state.FileGrants.clear();
			state.ImageCacheLayouts.clear();
			state.DirectoryGrants.clear();
			state.FileControls.clear();
			state.ExportUpdate = {};
			state.Playback.CurrentTick = 0;
			ApplyImageGraphTimeline(state.Authored, state.Playback);
			state.History.Clear();
			state.GroupHost.Clear();
			AuthoredDocumentChanged(state, detail::ImageGraphCacheEditKind::FreshDocument);
			state.ImportedPxcx.reset();
			state.ImportedCollectionFiles.reset();
			state.CollectionManagers.reset();
			state.PublishedPxcx = {};
			state.PxcxCompletedPreview.reset();
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
			CancelComposerPreview(state);
			state.AudioCapturePathDisplay = path.string();
			++state.EvaluationInputRevision;
			state.ComposerExports.Invalidate(state.DocumentRevision, state.EvaluationInputRevision);
			state.AudioCaptureMessage =
				"loaded " + std::to_string(state.AudioFrames.size()) + " recorded frames";
			state.LastDiagnostic = {};
			RequestPreview(state, true);
			return true;
		}

		bool OpenPxcx(State &state) {
			const std::filesystem::path path(state.PxcxPath);
			if (path.empty()) {
				state.PxcxOpenError = "enter a .pxcx, .pxcc or .pxz path";
				return false;
			}
			engine::imagegraphexport::PxcxSourceRead source;
			Diagnostic readDiagnostic;
			if (!engine::imagegraphexport::ReadPxcxSourceFile(
					path,
					state.Authored.Timeline.value_or(engine::imagegraph::TimelineSettings{}),
					source,
					readDiagnostic
				)) {
				state.PxcxOpenError = readDiagnostic.Message;
				return false;
			}
			auto &archive = source.Archive;
			engine::imagegraphio::PxcxImport imported;
			if (!engine::imagegraphio::ImportPxcxImageGraph(archive, imported, state.PxcxOpenError))
				return false;
			std::vector<engine::imagegraphio::PxcxCollectionMetadata> managers;
			Diagnostic managerDiagnostic;
			if (archive.MetadataNumber == 121092 && !engine::imagegraphio::PreparePxcxCollectionMetadata(
														archive,
														managers,
														managerDiagnostic,
														engine::imagegraph::Limits::MaximumEvaluationBytes,
														source.Collection.has_value()
													)) {
				state.PxcxOpenError = managerDiagnostic.Message;
				return false;
			}
			auto collections =
				std::make_shared<const std::vector<engine::imagegraphio::PxcxCollectionMetadata>>(
					std::move(managers)
				);

			engine::imagegraph::Diagnostic migrationDiagnostic;
			if (engine::imagegraph::Migrate(imported.Graph, migrationDiagnostic) != Status::Ok) {
				state.PxcxOpenError = migrationDiagnostic.Message.empty()
										  ? "imported image graph cannot be migrated"
										  : migrationDiagnostic.Message;
				return false;
			}

			const std::optional<engine::imagegraphio::PxcxReferencePreview> reference =
				imported.ReferencePreview();
			Image sourceThumbnail = std::move(source.Preview);
			if (sourceThumbnail.Pixels.empty() && reference.has_value() &&
				reference->Rgba.size() == engine::bake::PxcxLimits::ThumbnailRgbaBytes) {
				sourceThumbnail = {
					reference->Width,
					reference->Height,
					std::vector<uint8_t>(reference->Rgba.begin(), reference->Rgba.end()),
					reference->Hash
				};
			}
			state.CollectionManagers = std::move(collections);
			state.ImportedPxcx = std::move(imported.Source);
			state.ImportedCollectionFiles = std::move(source.Collection);
			state.PublishedPxcx = {};
			state.PxcxCompletedPreview.reset();
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
			ClearFeedbackHost(state);
			state.SourceCommonInitialState = engine::imagegraph::SourceNodeInitialState::Loaded;
			state.ExportGrants.clear();
			state.FileGrants.clear();
			state.ImageCacheLayouts.clear();
			state.DirectoryGrants.clear();
			state.FileControls.clear();
			state.ExportUpdate = {};
			state.Playback.CurrentTick = 0;
			ApplyImageGraphTimeline(state.Authored, state.Playback);
			AuthoredDocumentChanged(state, detail::ImageGraphCacheEditKind::FreshDocument);
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
			if (state.HaveGoodPreview && state.PreviewHash == engine::imagegraph::SurfaceHash(image) &&
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
			state.PreviewHash = engine::imagegraph::SurfaceHash(image);
			state.PreviewWidth = image.Width;
			state.PreviewHeight = image.Height;
			state.PreviewPixelBytes = image.Pixels.size();
			state.PreviewSourceFormat = image.Format;
			state.HaveGoodPreview = true;
			state.SinkMessage.clear();
			return true;
		}

		bool UploadPxcxReferenceThumbnail(State &state, engine::render::Renderer &renderer) {
			const Image &image = state.PublishedPxcx.Archive.OriginalBytes.empty()
									 ? state.PxcxReferenceThumbnail
									 : state.PublishedPxcx.ReferencePreview;
			const uint64_t bytes = uint64_t(image.Width) * image.Height * 4;
			if (!image.Width || !image.Height || image.Width > engine::imagegraph::Limits::MaximumDimension ||
				image.Height > engine::imagegraph::Limits::MaximumDimension ||
				bytes > engine::imagegraph::Limits::MaximumOutputBytes ||
				image.Format != engine::imagegraph::SurfaceFormat::RGBA8Unorm ||
				image.Pixels.size() != bytes) {
				state.PxcxThumbnailMessage = "source preview has invalid RGBA8 dimensions";
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

		std::string SelectedNodeId(const State &state);

		void RefreshPreview(State &state, engine::render::Renderer &renderer) {
			if (state.ExportIntent.Current) return;
			const auto selectedAxisNode = SelectedNodeId(state);
			if (state.AxisSelectedNodeId != selectedAxisNode) {
				state.AxisSelectedNodeId = selectedAxisNode;
				state.AxisObservations = {};
				RequestPreview(state, true);
			}
			if (!RetryCacheEdit(state) || !state.PreviewDirty ||
				(!state.LivePreview && !state.PreviewRequested))
				return;
			if (state.PxcxPreviewCacheInputRevision != state.EvaluationInputRevision) {
				state.PreviewCache.Clear();
				state.PreviewSequence.Invalidate();
				state.PxcxCompletedPreview.reset();
				state.PxcxPreviewCacheInputRevision = state.EvaluationInputRevision;
			}
			ImageGraphPlayback previewPlayback = state.Playback;
			const auto *composer = dynamic_cast<detail::ImageGraphComposerHost *>(state.Host.Composer);
			engine::imagegraph::EvaluationRequest observations;
			BindObservations(state, observations);
			const auto identity = state.ComposerCadence.RetainProgressPulse(
				detail::ImageGraphComposerCadence::Identity{
					composer ? composer->Owner : engine::core::Name{},
					state.SelectedOutput,
					state.DocumentRevision,
					state.EvaluationInputRevision,
					detail::ImageGraphPlaybackObservation(state.Authored, state.Playback)
				}
			);
			if (state.ComposerCadence.Current != identity) CancelComposerPreview(state, renderer);
			const auto previewFrame = state.ComposerCadence.Begin(
				identity,
				GetImageGraphFrame(state.Playback),
				state.PcxObservations,
				state.Playback.Playing,
				state.Playback.FrameProgress
			);
			(void)SetImageGraphAuthorFrame(previewPlayback, previewFrame);
			previewPlayback.Playing = state.ComposerCadence.Playing;
			previewPlayback.FrameProgress = state.ComposerCadence.FrameProgress;
			state.Host.LuaReceipts.Begin(previewFrame);
			state.PreviewObservations.Begin(previewFrame);
			const detail::ImageGraphLuaReceiptScope luaReceipts(state.Host.LuaReceipts);
			if (auto *host = dynamic_cast<detail::ImageGraphComposerHost *>(state.Host.Composer))
				host->Pending = false;
			struct FinishCapture {
				State &StateRef;
				engine::render::Renderer &Renderer;
				engine::imagegraph::FrameTime Frame;
				~FinishCapture() {
					const auto *host = dynamic_cast<detail::ImageGraphComposerHost *>(StateRef.Host.Composer);
					if (host && host->Pending) {
						StateRef.ComposerCadence.Pending();
						StateRef.PreviewDirty = StateRef.PreviewRequested = true;
					} else if (StateRef.LastDiagnostic.Code == Status::Ok) {
						StateRef.ComposerCadence.Complete(Frame);
						if (Frame != GetImageGraphFrame(StateRef.Playback))
							StateRef.PreviewDirty = StateRef.PreviewRequested = true;
					} else
						CancelComposerPreview(StateRef, Renderer);
				}
			} finish{state, renderer, previewFrame};
			detail::ImageGraphAxisProcessingObservers axisCandidate;
			const std::string axisNode = SelectedNodeId(state);
			const uint64_t axisRevision = state.DocumentRevision,
						   axisInputRevision = state.EvaluationInputRevision;
			bool axisEvaluated = false;
			struct FinishAxisObservation {
				State &StateRef;
				detail::ImageGraphAxisProcessingObservers &Candidate;
				const std::string &NodeId;
				uint64_t Revision, InputRevision;
				engine::imagegraph::FrameTime Frame;
				bool &Evaluated;
				~FinishAxisObservation() {
					const auto *host = dynamic_cast<detail::ImageGraphComposerHost *>(StateRef.Host.Composer);
					bool valid = StateRef.LastDiagnostic.Code == Status::Ok && !(host && host->Pending) &&
								 SelectedNodeId(StateRef) == NodeId &&
								 StateRef.DocumentRevision == Revision &&
								 StateRef.EvaluationInputRevision == InputRevision &&
								 GetImageGraphFrame(StateRef.Playback) == Frame;
					for (const auto &owner : Candidate.Owners)
						valid = valid && owner.Expected.GroupRevision ==
											 StateRef.GroupHost.Replay.ObservationRevision();
					if (!valid)
						StateRef.AxisObservations = {};
					else if (Evaluated) {
						if (Candidate.Failure != Status::Ok) StateRef.AxisReceiptFailure = Candidate.Failure;
						detail::PublishImageGraphAxisProcessingObservers(
							StateRef.AxisObservations,
							std::move(Candidate),
							true,
							NodeId,
							Revision,
							InputRevision,
							Frame
						);
					}
				}
			} finishAxes{
				state, axisCandidate, axisNode, axisRevision, axisInputRevision, previewFrame, axisEvaluated
			};
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
						"PXCX has no projected image output; the "
						"original source archive remains available"
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
			const uint64_t sequenceHeld = state.PreviewSequence.RetainedBytes();
			const auto valueHeld = engine::imagegraph::ValueClonePayloadBytes(state.ValuePreview);
			const auto arrayHeld = engine::imagegraph::ValueClonePayloadBytes(state.ArrayPreview);
			const uint64_t cacheBytes = state.PreviewCache.RetainedBytes();
			if (!valueHeld || !arrayHeld || *valueHeld > UINT64_MAX - *arrayHeld ||
				cacheBytes > UINT64_MAX - *valueHeld - *arrayHeld) {
				state.LastDiagnostic = {
					Status::LimitExceeded, {}, {}, "Retained typed preview storage exceeds bounds"
				};
				return;
			}
			bool staleAxisMaps = false;
			for (const auto &owner : state.AxisObservations.Owners)
				staleAxisMaps = staleAxisMaps || owner.Expected.NodeId != axisNode ||
								owner.Expected.AuthoringRevision != axisRevision ||
								owner.Expected.InputRevision != axisInputRevision ||
								owner.Expected.Frame != previewFrame ||
								owner.Expected.GroupRevision != state.GroupHost.Replay.ObservationRevision();
			if (staleAxisMaps) {
				state.AxisObservations = {};
				state.AxisReceiptFailure = Status::Ok;
			}
			uint64_t axisHeld = state.AxisObservations.RetainedBytes();
			const uint64_t previewLimit = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (axisHeld > previewLimit - std::min(cacheBytes + *valueHeld + *arrayHeld, previewLimit)) {
				state.AxisObservations = {};
				axisHeld = 0;
				state.AxisReceiptFailure = Status::LimitExceeded;
			}
			uint64_t cacheHeld = cacheBytes + *valueHeld + *arrayHeld + axisHeld;
			const uint64_t keyHeld = sizeof(detail::ImageGraphPreviewSequence::Key) +
									 identity.Output.capacity() +
									 IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES +
									 sizeof(engine::assets::TextureData) + 2 * sizeof(std::vector<std::byte>);
			const uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (axisHeld && (sequenceHeld > maximumBytes || cacheHeld > maximumBytes - sequenceHeld ||
							 keyHeld >= maximumBytes - sequenceHeld - cacheHeld)) {
				state.AxisObservations = {};
				cacheHeld -= axisHeld;
				axisHeld = 0;
				state.AxisReceiptFailure = Status::LimitExceeded;
			}
			if (sequenceHeld > maximumBytes || cacheHeld > maximumBytes - sequenceHeld ||
				keyHeld >= maximumBytes - sequenceHeld - cacheHeld) {
				state.LastDiagnostic = {
					Status::LimitExceeded, {}, {}, "Retained preview leaves no result allowance"
				};
				return;
			}
			uint64_t previewAllowance = maximumBytes - sequenceHeld - cacheHeld - keyHeld;
			detail::ImageGraphPreviewSequence::Key sequenceKey{
				identity,
				previewFrame,
				detail::ImageGraphPlaybackObservation(previewDocument, previewPlayback)
			};
			bool axisReceiptCurrent = true;
			const auto *axisSelected = FindNode(state.Authored, axisNode);
			const auto *axisSchema =
				axisSelected ? engine::imagegraph::FindSchema(axisSelected->Type) : nullptr;
			bool hasAxisControl = false;
			if (axisSchema)
				for (const auto &port : axisSchema->Ports)
					if (port.Direction == engine::imagegraph::PortDirection::Input &&
						engine::imagegraph::SupportsSourceAxisTransition(*axisSelected, port.Id))
						hasAxisControl = true;
			if (hasAxisControl) {
				axisReceiptCurrent = !state.AxisObservations.Owners.empty();
				for (const auto &owner : state.AxisObservations.Owners)
					axisReceiptCurrent =
						axisReceiptCurrent && owner.Candidate.Identity && owner.Expected.NodeId == axisNode &&
						owner.Expected.AuthoringRevision == axisRevision &&
						owner.Expected.InputRevision == axisInputRevision &&
						owner.Expected.Frame == previewFrame &&
						owner.Expected.GroupRevision == state.GroupHost.Replay.ObservationRevision();
			}
			if (axisReceiptCurrent && state.PreviewSequence.Matches(sequenceKey)) {
				const auto selected = state.PreviewSequence.Selected();
				if (selected.Image && !UploadPreview(state, renderer, *selected.Image)) return;
				if (!selected.Image) state.HaveGoodPreview = false;
				state.LastDiagnostic = {};
				return;
			}

			const PxcxPreviewIdentity completedPreview{
				state.DocumentRevision,
				state.EvaluationInputRevision,
				*selectedOutput,
				previewFrame,
				detail::ImageGraphPlaybackObservation(previewDocument, previewPlayback)
			};
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
				if (const Image *cached =
						axisReceiptCurrent
							? state.PreviewCache.Find(
								  state.DocumentRevision,
								  outputIndex,
								  previewPlayback.CurrentTick,
								  previewPlayback.Subframe,
								  previewPlayback.NegativeFrame,
								  detail::ImageGraphPlaybackObservation(previewDocument, previewPlayback)
							  )
							: nullptr) {
					if (!UploadPreview(state, renderer, *cached)) return;
					state.PreviewSequence.Clear();
					state.PxcxCompletedPreview = completedPreview;
					state.LastDiagnostic = {};
					return;
				}
			} else {
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
				engine::imagegraphphysics::RigidProvider requestRigidProvider;
				engine::imagegraph::EvaluationRequest request;
				request.SourceSafeMode = state.SourceSafeMode;
				detail::ImageGraphPreviewProvider previewProvider(
					HostFor(state), state.PreviewObservations, request
				);
				request.HostProvider = &previewProvider;
				request.MaximumImageDimension = IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
				state.ComposerCadence.Observations.Bind(request);
				detail::BindImageGraphRigid(request, requestRigidProvider, previewPlayback);
				(void)engine::imagegraph::SetFrameTime(request, previewFrame);
				request.AudioFrames =
					std::span<const engine::imagegraph::AudioCaptureFrame>(state.AudioFrames);
				request.AudioClips = state.AudioClips;
				engine::imagegraph::SourceFontContext heldFontContext;
				if (state.FontInputsActive) {
					if (!detail::BindImageGraphFontInputs(
							*state.FontInputs,
							true,
							previewPlayback.Playing,
							heldFontContext,
							request,
							engine::imagegraph::Limits::MaximumEvaluationBytes,
							diagnostic
						)) {
						state.LastDiagnostic = std::move(diagnostic);
						return;
					}
				}
				if (!state.GroupHost.Prepare(
						previewDocument, plan, state.DocumentRevision, request, diagnostic
					)) {
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				if (!state.PendingGroupRender.empty()) {
					if (!state.FeedbackHost.ForceGroup(
							previewDocument,
							plan,
							state.PendingGroupRender,
							request,
							diagnostic,
							previewAllowance
						)) {
						state.LastDiagnostic = std::move(diagnostic);
						return;
					}
					state.PendingGroupRender.clear();
				}
				if (!detail::PrepareImageGraphAxisProcessingObservers(
						previewDocument,
						plan,
						state.GroupHost.Replay,
						axisNode,
						axisRevision,
						axisInputRevision,
						previewFrame,
						axisCandidate,
						diagnostic,
						previewAllowance
					)) {
					state.AxisReceiptFailure = diagnostic.Code;
					axisCandidate = {};
					diagnostic = {};
				}
				request.SourceInputObserver = axisCandidate.Owners.empty() ? nullptr : &axisCandidate;
				axisEvaluated = true;
				studio::ImageGraphPreviewValue preview;
				const auto status = detail::PrepareImageGraphPreviewResult(
					previewDocument,
					plan,
					state.SelectedOutput,
					request,
					state.FeedbackHost,
					state.DocumentRevision,
					state.EvaluationInputRevision,
					preview,
					diagnostic,
					previewAllowance
				);

				request.SourceInputObserver = nullptr;

				if (state.LuaHost) {
					auto messages = state.LuaHost->TakeMessages();
					if (!messages.empty()) state.LuaMessages = std::move(messages);
				}
				if (status == Status::SourceAxisInitializationRequired && composer) {
					// refused evaluation no longer needs its plan while the constructor clone is live.
					plan = {};
					const auto fontContextBytes = SourceFontContextRetainedBytes(heldFontContext);
					const auto allowance = GroupConstructorAllowance(state, keyHeld);
					if (!allowance || !fontContextBytes || *fontContextBytes >= *allowance) {
						state.LastDiagnostic = {
							Status::LimitExceeded,
							{},
							{},
							"retained Studio owners leave no source constructor allowance"
						};
						return;
					}
					if (!state.GroupHost.RetainAxisRead(
							previewDocument,
							state.DocumentRevision,
							request,
							diagnostic,
							diagnostic,
							*allowance - *fontContextBytes
						)) {
						state.LastDiagnostic = std::move(diagnostic);
						return;
					}
					// one event per retained pulse keeps constructor retries out of the synchronous frame
					// loop.
					state.LastDiagnostic = {};
					state.PreviewDirty = state.PreviewRequested = true;
					if (auto *host = dynamic_cast<detail::ImageGraphComposerHost *>(state.Host.Composer))
						host->Pending = true;
					return;
				}
				if (status != Status::Ok) {
					state.LastDiagnostic = std::move(diagnostic);
					return;
				}
				if (auto *sequence = std::get_if<engine::imagegraph::ImageArray>(&preview)) {
					const uint64_t feedbackHeld = state.FeedbackHost.RetainedBytes();
					if (feedbackHeld > maximumBytes - cacheHeld - keyHeld ||
						!state.PreviewSequence.Admit(
							*sequence,
							sequenceKey,
							maximumBytes - cacheHeld - keyHeld - feedbackHeld,
							diagnostic
						)) {
						state.LastDiagnostic =
							diagnostic.Code == Status::Ok
								? Diagnostic{Status::LimitExceeded, {}, {}, "Retained feedback leaves no sequence allowance"}
								: std::move(diagnostic);
						return;
					}
					const auto selected = state.PreviewSequence.SelectReplacement(*sequence, sequenceKey);
					if (selected.Image && !UploadPreview(state, renderer, *selected.Image)) return;
					state.PreviewSequence.Publish(std::move(*sequence), std::move(sequenceKey));
					state.HaveScalarPreview = state.HaveArrayPreview = false;
					if (!selected.Image) state.HaveGoodPreview = false;
					state.PxcxCompletedPreview = completedPreview;
					state.LastDiagnostic = {};
					return;
				}

				if (auto *value = std::get_if<engine::imagegraph::EvaluatedValue>(&preview)) {
					state.PreviewSequence.Clear();
					state.HaveGoodPreview = false;
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
				state.PreviewSequence.Clear();
				const bool retainedPreview = state.PreviewCache.Store(
					state.DocumentRevision,
					outputIndex,
					previewPlayback.CurrentTick,
					*image,
					previewPlayback.Subframe,
					previewPlayback.NegativeFrame,
					detail::ImageGraphPlaybackObservation(previewDocument, previewPlayback)
				);
				if (retainedPreview)
					state.PxcxCompletedPreview = completedPreview;
				else
					state.PxcxCompletedPreview.reset();
				state.LastDiagnostic = {};
			}
		}

		bool AppendPxcx(State &state) try {
			using namespace engine::imagegraph;
			using namespace engine::imagegraphio;
			if (!state.ImportedPxcx || !RetryCacheEdit(state)) return false;
			const auto fail = [&](Status status, const char *message) {
				state.LastDiagnostic = {status, {}, {}, message};
				state.PxcxOpenError = message;
				return false;
			};
			if (state.HaveActiveEdit || state.Playback.Rendering || state.RangeExport)
				return fail(Status::InvalidValue, "finish the active edit or render before appending");
			const std::filesystem::path path(state.PxcxAppendPath);
			if (path.empty()) return fail(Status::InvalidValue, "enter a .pxcx, .pxcc or .pxz append path");
			PxcxAppendOptions options;
			options.Namespace = state.PxcxAppendNamespace;
			options.Context = state.PxcxAppendContext;
			options.Offset = {state.PxcxAppendOffset[0], state.PxcxAppendOffset[1]};
			if (state.Canvas.Inside() != nodegraph::NO_NODE) {
				const auto context = state.Ids.ToDocument.find(state.Canvas.Inside());
				if (context == state.Ids.ToDocument.end())
					return fail(Status::InvalidValue, "canvas append context has no source identity");
				options.Context = context->second;
			}
			engine::imagegraphexport::PxcxSourceRead read;
			Diagnostic readDiagnostic;
			if (!engine::imagegraphexport::ReadPxcxSourceFile(
					path,
					state.Authored.Timeline.value_or(engine::imagegraph::TimelineSettings{}),
					read,
					readDiagnostic,
					Limits::MaximumEvaluationBytes,
					false
				)) {
				state.PxcxOpenError = readDiagnostic.Message;
				return false;
			}
			engine::bake::PxcxArchive incoming = std::move(read.Archive);
			read = {};
			const auto allowance = GroupConstructorAllowance(state);
			if (!allowance) return fail(Status::LimitExceeded, "retained owners leave no append allowance");
			uint64_t remaining = *allowance;
			const auto charge = [&](std::optional<uint64_t> amount) {
				if (!amount || *amount >= remaining) return false;
				remaining -= *amount;
				return true;
			};
			if (!charge(DocumentRetainedPayloadBytes(state.Authored)) ||
				!charge(DocumentRetainedPayloadBytes(state.PxcxProjection)) ||
				!charge(detail::ImageGraphHistoryCanvasBytes(state.Graph, state.Ids)) ||
				!charge(state.GroupHost.Replay.RetainedBytes()) ||
				!charge(DocumentRetainedPayloadBytes(state.CacheEditObservation.Inputs)) ||
				!charge(detail::ImageGraphHistoryArchiveBytes(incoming)) ||
				!charge(detail::ImageGraphHistoryArchiveBytes(*state.ImportedPxcx)))
				return fail(Status::LimitExceeded, "append host owners exceed live bytes");
			// grug private observations and host keep refused callbacks out of live state.
			detail::ImageGraphAppendHost host;
			host.Arguments = &state.Host.SourceArguments;
			host.Files.Grants = state.FileGrants;
			host.Files.Directories = state.DirectoryGrants;
			host.Files.ImageCaches = state.ImageCacheLayouts;
			EvaluationRequest clock;
			clock.HostProvider = &host;
			clock.AudioFrames = state.AudioFrames;
			clock.AudioClips = state.AudioClips;
			detail::ImageGraphObservations observations = state.PcxObservations;
			const auto frame = GetImageGraphFrame(state.Playback);
			const std::string project = std::filesystem::path(state.PxcxPathDisplay).stem().string();
			if (!observations.Matches(state.DocumentRevision, frame, project)) {
				const auto now = std::time(nullptr);
				std::tm calendar{};
#ifdef _WIN32
				localtime_s(&calendar, &now);
#else
				localtime_r(&now, &calendar);
#endif
				observations.Capture(
					state.DocumentRevision,
					frame,
					project,
					std::chrono::duration<double>(std::chrono::steady_clock::now() - state.SessionStart)
						.count(),
					calendar
				);
			}
			observations.Bind(clock);
			SourceFontContext fonts;
			if (state.FontInputsActive && !detail::BindImageGraphFontInputs(
											  *state.FontInputs,
											  true,
											  state.Playback.Playing,
											  fonts,
											  clock,
											  remaining,
											  state.LastDiagnostic
										  ))
				return false;
			if (!charge(SourceFontContextRetainedBytes(fonts)))
				return fail(Status::LimitExceeded, "append font owners exceed live bytes");
			engine::imagegraphphysics::RigidProvider rigid;
			detail::BindImageGraphRigid(clock, rigid, state.Playback);
			(void)SetFrameTime(clock, frame);
			Document callbacks;
			if (PrepareGroupCallbackDocument(state.Authored, callbacks, state.LastDiagnostic, remaining) !=
				Status::Ok)
				return false;
			detail::AddImageGraphAppendCallbackOutput(callbacks);
			const auto callbackBytes = DocumentRetainedPayloadBytes(callbacks);
			if (!charge(callbackBytes))
				return fail(Status::LimitExceeded, "append callback document exceeds live bytes");
			ImageGraphGroupHost prepared;
			if (RebindGroupReplay(
					callbacks,
					state.GroupHost.Replay,
					state.DocumentRevision,
					prepared.Replay,
					state.LastDiagnostic,
					remaining
				) != Status::Ok)
				return false;
			prepared.Revision = state.GroupHost.Revision;
			prepared.BorrowedBytes = Limits::MaximumEvaluationBytes - remaining;
			if (callbacks.Nodes.empty())
				prepared.Revision = state.DocumentRevision;
			else {
				Plan plan;
				const uint64_t planBytes =
					prepared.Budget(state.LastDiagnostic, {&callbacks}, {&prepared.Replay}) / 2;
				if (!planBytes || Compile(callbacks, plan, state.LastDiagnostic, planBytes) != Status::Ok)
					return false;
				prepared.BorrowedBytes += planBytes;
				if (!prepared.Prepare(callbacks, plan, state.DocumentRevision, clock, state.LastDiagnostic))
					return false;
				prepared.BorrowedBytes -= planBytes;
			}
			prepared.BorrowedBytes = 0;
			callbacks = {};
			remaining += *callbackBytes;
			if (!charge(prepared.Replay.RetainedBytes()))
				return fail(Status::LimitExceeded, "append prepared Group owner exceeds live bytes");
			Document saved;
			if (state.Authored.Nodes.empty())
				saved = state.Authored;
			else if (!prepared.ProjectForSave(
						 state.Authored, state.DocumentRevision, saved, state.LastDiagnostic, remaining
					 ))
				return false;
			const auto savedBytes = DocumentRetainedPayloadBytes(saved);
			if (!charge(savedBytes))
				return fail(Status::LimitExceeded, "append save projection exceeds live bytes");
			PxcxImport imported;
			std::string error;
			PxcxImportOptions importOptions;
			importOptions.MaximumOperationBytes = remaining;
			if (!ImportPxcxImageGraph(*state.ImportedPxcx, imported, error, importOptions)) {
				state.PxcxOpenError = std::move(error);
				return false;
			}
			engine::bake::PxcxArchive destination;
			if (!detail::PrepareImageGraphAppendDestination(
					imported, saved, frame, destination, state.LastDiagnostic, remaining
				))
				return false;
			imported = {};
			saved = {};
			remaining += *savedBytes;
			if (!charge(host.Files.RetainedObservationBytes()))
				return fail(Status::LimitExceeded, "append callback owners exceed live bytes");
			const uint64_t revision = state.DocumentRevision == std::numeric_limits<uint64_t>::max()
										  ? 1
										  : state.DocumentRevision + 1;
			detail::ImageGraphAppendProject candidate;
			if (!candidate.Prepare(
					*state.ImportedPxcx,
					destination,
					incoming,
					std::move(options),
					path.string(),
					state.Authored,
					prepared,
					state.CollectionManagers,
					state.Ids,
					revision,
					clock,
					state.LastDiagnostic,
					remaining
				)) {
				state.PxcxOpenError = state.LastDiagnostic.Message;
				return false;
			}
			nodegraph::Canvas canvas = state.Canvas;
			canvas.Ascend(candidate.Graph, 0);
			canvas.Select(std::move(candidate.Selection));
			if (!candidate.Admit(
					state.Authored,
					state.History,
					state.FeedbackHost,
					state.CollectionManagers,
					state.LastDiagnostic
				)) {
				state.PxcxOpenError = state.LastDiagnostic.Message;
				return false;
			}
			static_assert(std::is_nothrow_move_assignable_v<nodegraph::Canvas>);
			static_assert(std::is_nothrow_move_assignable_v<detail::ImageGraphAppendProject>);
			CancelComposerPreview(state);
			state.ImportedPxcx = std::move(candidate.Source);
			state.PxcxProjection = std::move(candidate.Projection);
			state.PxcxDiagnostics = std::move(candidate.Diagnostics);
			state.Authored = std::move(candidate.Groups.Authored);
			state.Graph = std::move(candidate.Graph);
			state.Ids = std::move(candidate.Ids);
			state.Canvas = std::move(canvas);
			if (!candidate.SelectedGroups.empty()) {
				state.SelectedGroup = std::move(candidate.SelectedGroups.front());
				std::fill(state.GroupName.begin(), state.GroupName.end(), '\0');
				const auto selected = std::find_if(
					state.Authored.Groups.begin(), state.Authored.Groups.end(), [&](const auto &group) {
						return group.Id == state.SelectedGroup;
					}
				);
				if (selected != state.Authored.Groups.end())
					std::copy_n(
						selected->Name.c_str(),
						std::min(selected->Name.size(), state.GroupName.size() - 1),
						state.GroupName.data()
					);
			}
			state.CacheEditObservation = std::move(candidate.Observation);
			state.CollectionManagers = std::move(candidate.Collections);
			state.PublishedPxcx = {};
			state.CanvasNeedsReload = false;
			state.AdapterError.clear();
			state.PxcxOpenError.clear();
			state.CacheEditBlocked = false;
			PublishAuthoredDocumentChanged(state);
			state.GroupHost = std::move(candidate.Groups.Host);
			RequestPreview(state, true);
			return true;
		} catch (const std::bad_alloc &) {
			state.LastDiagnostic = {Status::LimitExceeded, {}, {}, "Studio append allocation refused"};
			state.PxcxOpenError = state.LastDiagnostic.Message;
			return false;
		}

		bool SavePxcx(State &state, bool withPreview) {
			Diagnostic diagnostic;
			Document projected;
			const Document *savedDocument = &state.Authored;
			if (state.GroupHost.Revision == state.DocumentRevision &&
				state.GroupHost.Replay.InstancesBound()) {
				const auto allowance = GroupConstructorAllowance(state);
				if (!allowance ||
					!state.GroupHost.ProjectForSave(
						state.Authored, state.DocumentRevision, projected, diagnostic, allowance.value_or(0)
					)) {
					state.PxcxOpenError = diagnostic.Message.empty()
											  ? "retained owners leave no source save allowance"
											  : diagnostic.Message;
					return false;
				}
				savedDocument = &projected;
			}

			if (!withPreview && state.PublishedPxcx.Archive.OriginalBytes.empty()) {
				const bool saved = SavePxcxProjection(
					std::filesystem::path(state.PxcxPath),
					*state.ImportedPxcx,
					*savedDocument,
					GetImageGraphFrame(state.Playback),
					diagnostic
				);
				state.PxcxOpenError = saved ? std::string{} : diagnostic.Message;
				return saved;
			}
			std::optional<PxcxPreviewIdentity> current;
			const Image *pixels = nullptr;
			if (withPreview) {
				const auto selected = std::find_if(
					state.Authored.Outputs.begin(), state.Authored.Outputs.end(), [&](const Output &output) {
						return output.Id == state.SelectedOutput;
					}
				);
				if (selected != state.Authored.Outputs.end()) {
					current = PxcxPreviewIdentity{
						state.DocumentRevision,
						state.EvaluationInputRevision,
						*selected,
						GetImageGraphFrame(state.Playback),
						detail::ImageGraphPlaybackObservation(state.Authored, state.Playback)
					};
					if (state.PxcxCompletedPreview == current && !state.PreviewDirty &&
						state.LastDiagnostic.Code == Status::Ok) {
						pixels = state.PreviewCache.Find(
							state.DocumentRevision,
							size_t(selected - state.Authored.Outputs.begin()),
							current->Frame.Tick,
							current->Frame.Subframe,
							current->Frame.NegativeFrame,
							current->PlaybackObservation
						);
					}
				}
				if (!pixels) {
					state.PxcxOpenError = "Refresh the selected image preview before saving its thumbnail.";
					return false;
				}
			}
			std::optional<PxcxPreparedSavePreview> prepared;
			if (pixels) prepared.emplace(*pixels, *state.PxcxCompletedPreview, *current);
			if (!SavePxcxProjectionAndAdopt(
					std::filesystem::path(state.PxcxPath),
					*state.ImportedPxcx,
					*savedDocument,
					GetImageGraphFrame(state.Playback),
					state.PublishedPxcx,
					prepared ? &*prepared : nullptr,
					diagnostic
				)) {
				state.PxcxOpenError = diagnostic.Message;
				return false;
			}
			state.PxcxReferenceThumbnail = {};
			state.PxcxOpenError.clear();
			return true;
		}

		bool SaveCollection(State &state) try {
			using namespace engine::imagegraphio;
			ENGINE_PROFILE("studio.imagegraph.collection_save_action");
			Diagnostic diagnostic;
			const auto fail = [&](Status code, std::string message) {
				state.LastDiagnostic = {code, {}, {}, std::move(message)};
				state.PxcxOpenError = state.LastDiagnostic.Message;
				return false;
			};
			if (!state.ImportedPxcx || !RetryCacheEdit(state)) return false;
			if (state.HaveActiveEdit || state.Playback.Rendering || state.RangeExport)
				return fail(
					Status::InvalidValue, "finish the active edit or render before saving a collection"
				);
			const std::filesystem::path destination(state.PxcxCollectionPath);
			if (destination.empty() ||
				(destination.extension() != ".pxcc" && destination.extension() != ".pxz"))
				return fail(Status::InvalidValue, "enter a .pxcc or .pxz collection path");
			std::optional<PxcxPreviewIdentity> currentPreview;
			const Image *previewPixels = nullptr;
			if (state.PxcxCollectionPreview) {
				const auto selected = std::find_if(
					state.Authored.Outputs.begin(), state.Authored.Outputs.end(), [&](const Output &output) {
						return output.Id == state.SelectedOutput;
					}
				);
				if (selected != state.Authored.Outputs.end()) {
					currentPreview = PxcxPreviewIdentity{
						state.DocumentRevision,
						state.EvaluationInputRevision,
						*selected,
						GetImageGraphFrame(state.Playback),
						detail::ImageGraphPlaybackObservation(state.Authored, state.Playback)
					};
					if (state.PxcxCompletedPreview == currentPreview && !state.PreviewDirty &&
						state.LastDiagnostic.Code == Status::Ok)
						previewPixels = state.PreviewCache.Find(
							state.DocumentRevision,
							size_t(selected - state.Authored.Outputs.begin()),
							currentPreview->Frame.Tick,
							currentPreview->Frame.Subframe,
							currentPreview->Frame.NegativeFrame,
							currentPreview->PlaybackObservation
						);
				}
				if (!previewPixels)
					return fail(
						Status::InvalidValue,
						"refresh the selected image preview before saving the collection"
					);
			}
			const auto manager =
				state.CollectionManagers
					? std::find_if(
						  state.CollectionManagers->begin(),
						  state.CollectionManagers->end(),
						  [&](const auto &item) { return item.NodeId == state.PxcxCollectionId; }
					  )
					: std::vector<PxcxCollectionMetadata>::const_iterator{};
			if (!state.CollectionManagers || manager == state.CollectionManagers->end())
				return fail(Status::InvalidValue, "select a source collection to save");
			const auto allowance = GroupConstructorAllowance(state);
			if (!allowance)
				return fail(Status::LimitExceeded, "retained owners leave no collection save allowance");
			uint64_t remaining = *allowance;
			const auto charge = [&](std::optional<uint64_t> bytes) {
				if (!bytes || *bytes >= remaining) return false;
				remaining -= *bytes;
				return true;
			};
			if (!charge(detail::ImageGraphHistoryArchiveBytes(*state.ImportedPxcx)) ||
				!charge(DocumentRetainedPayloadBytes(state.Authored)) ||
				!charge(DocumentRetainedPayloadBytes(state.PxcxProjection)) ||
				!charge(detail::ImageGraphCollectionBytes(state.CollectionManagers)) ||
				!charge(detail::ImageGraphHistoryCanvasBytes(state.Graph, state.Ids)) ||
				!charge(state.GroupHost.Replay.RetainedBytes()))
				return fail(Status::LimitExceeded, "collection save owners exceed live bytes");
			Document projected;
			const Document *saved = &state.Authored;
			if (state.GroupHost.Replay.InstancesBound()) {
				if (!state.GroupHost.ProjectForSave(
						state.Authored, state.DocumentRevision, projected, diagnostic, remaining
					))
					return fail(diagnostic.Code, diagnostic.Message);
				saved = &projected;
			} else if (!state.Authored.Groups.empty()) {
				return fail(
					Status::InvalidValue, "refresh the collection preview before saving its constructor state"
				);
			}
			if (!charge(DocumentRetainedPayloadBytes(projected)))
				return fail(Status::LimitExceeded, "collection save projection exceeds live bytes");
			engine::bake::PxcxArchive checked;
			{
				const auto archiveBytes = detail::ImageGraphHistoryArchiveBytes(*state.ImportedPxcx);
				const auto documentBytes = DocumentRetainedPayloadBytes(*saved);
				if (!archiveBytes || !documentBytes || *archiveBytes >= remaining / 32 ||
					*documentBytes >= remaining / 32)
					return fail(Status::LimitExceeded, "collection save import scratch exceeds allowance");
				PxcxImport imported;
				PxcxImportOptions options;
				options.MaximumOperationBytes = remaining;
				std::string error;
				if (!ImportPxcxImageGraph(*state.ImportedPxcx, imported, error, options))
					return fail(Status::InvalidValue, std::move(error));
				if (!detail::PrepareImageGraphAppendDestination(
						imported, *saved, GetImageGraphFrame(state.Playback), checked, diagnostic, remaining
					))
					return fail(diagnostic.Code, diagnostic.Message);
			}
			std::optional<PxcxPreparedSavePreview> preparedPreview;
			if (previewPixels)
				preparedPreview.emplace(*previewPixels, *state.PxcxCompletedPreview, *currentPreview);
			if (!SavePxcxCollection(
					destination,
					checked,
					manager->NodeId,
					std::string_view(manager->MetadataJson),
					diagnostic,
					remaining,
					preparedPreview ? &*preparedPreview : nullptr
				))
				return fail(diagnostic.Code, diagnostic.Message);
			state.PxcxOpenError.clear();
			state.LastDiagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			state.PxcxOpenError = "collection save allocation refused";
			return false;
		} catch (const std::filesystem::filesystem_error &) {
			state.PxcxOpenError = "collection save path is invalid";
			return false;
		}

		void RunAnimationControls(State &state, engine::render::Renderer &renderer) {
			if (state.Playback.Rendering ||
				std::none_of(state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [](const auto &node) {
					return node.Type == "pc.animation_control";
				}))
				return;
			engine::imagegraph::Plan plan;
			Diagnostic error;
			if (engine::imagegraph::Compile(state.Authored, plan, error) != Status::Ok) {
				state.LastDiagnostic = std::move(error);
				return;
			}
			// Source controls step in authored order, independently of the selected
			// preview output.
			for (const auto &node : state.Authored.Nodes) {
				if (state.Playback.Rendering) break;
				if (node.Type != "pc.animation_control") continue;
				engine::imagegraphphysics::RigidProvider requestRigidProvider;
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				engine::imagegraph::SourceFontContext requestFontContext;
				if (!BindObservations(state, request, &requestFontContext)) continue;
				detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
				if (!state.GroupHost.Prepare(state.Authored, plan, state.DocumentRevision, request, error) ||
					!state.FeedbackHost.PrepareNodeInputs(
						state.Authored,
						plan,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						node.Id,
						request,
						error
					)) {
					state.LastDiagnostic = std::move(error);
					continue;
				}
				engine::imagegraph::AnimationControlResult result;
				const auto &snapshot = state.FeedbackHost.Snapshot();
				const auto resolved = snapshot.Values().empty()
										  ? engine::imagegraph::ResolveAnimationControl(
												state.Authored,
												plan,
												node.Id,
												request,
												detail::AnimationPlayback(state.Playback),
												engine::imagegraph::Limits::MaximumEvaluationBytes,
												result,
												error
											)
										  : engine::imagegraph::ResolveAnimationControl(
												snapshot,
												node.Id,
												detail::AnimationPlayback(state.Playback),
												engine::imagegraph::Limits::MaximumEvaluationBytes,
												result,
												error
											);
				if (resolved != Status::Ok) {
					state.LastDiagnostic = std::move(error);
					continue;
				}
				if (!detail::ApplyAnimationControl(
						state.Playback,
						result,
						[&](auto effect) {
							using Kind = engine::imagegraph::AnimationControlEffectKind;
							if (effect == Kind::AnimationStart) {
								detail::RestartAnimationReplay(state.FeedbackHost, state.PreviewCache);
								state.PreviewSequence.Invalidate();
							}
							if (effect == Kind::RenderAll) {
								state.PreviewCache.Clear();
								state.PreviewSequence.Invalidate();
								RequestPreview(state, true);
								RefreshPreview(state, renderer);
							}
						},
						error
					))
					state.LastDiagnostic = std::move(error);
				if (state.Playback.Rendering && state.Playback.FrameProgress) RequestPreview(state, true);
			}
		}

		void ApplyHistory(State &state, bool redo) try {
			if (!RetryCacheEdit(state)) return;
			detail::ImageGraphHistorySource source;
			nodegraph::Graph graph;
			ImageGraphCanvasIds ids;
			std::vector<nodegraph::NodeId> selection;
			std::string output;
			const detail::ImageGraphHistoryPreparation prepare =
				[&](const Document &,
					const Document &restored,
					const ImageGraphHistory::SourceSnapshot &currentSource,
					const ImageGraphHistory::SourceSnapshot &restoredSource,
					uint64_t &remaining) {
					const auto charge = [&](std::optional<uint64_t> bytes) {
						if (!bytes || *bytes >= remaining) {
							state.LastDiagnostic = {
								Status::LimitExceeded,
								{},
								{},
								"Studio history owners exceed live payload bounds"
							};
							return false;
						}
						remaining -= *bytes;
						return true;
					};
					if (!charge(engine::imagegraph::DocumentRetainedPayloadBytes(state.PxcxProjection)) ||
						!charge(detail::ImageGraphHistoryCanvasBytes(state.Graph, state.Ids)) ||
						!charge(state.GroupHost.Replay.RetainedBytes()) ||
						!charge(state.TimelineRead.RetainedBytes()) ||
						!charge(detail::ImageGraphHistoryDiagnosticBytes(state.PxcxDiagnostics)) ||
						!charge(state.SelectedOutput.capacity()) ||
						!charge(state.Canvas.Selection().capacity() * sizeof(nodegraph::NodeId)) ||
						!charge(state.PxcxReferenceThumbnail.Pixels.capacity()))
						return false;
					if (state.ImportedPxcx &&
						!charge(detail::ImageGraphHistoryArchiveBytes(*state.ImportedPxcx)))
						return false;
					const auto restoredBytes = engine::imagegraph::DocumentRetainedPayloadBytes(restored);
					const auto observerBytes =
						engine::imagegraph::DocumentRetainedPayloadBytes(state.CacheEditObservation.Inputs);
					const auto beforeBorrowed = remaining;
					if (!charge(restoredBytes) || !charge(observerBytes) ||
						!charge(state.FeedbackHost.RetainedBytes()))
						return false;
					const auto borrowed = beforeBorrowed - remaining;
					if (!source.Prepare(
							currentSource, restoredSource, state.ImportedPxcx, remaining, state.LastDiagnostic
						))
						return false;
					std::string error;
					if (!LoadImageGraphCanvas(restored, graph, ids, error)) {
						state.LastDiagnostic = {Status::InvalidValue, {}, {}, std::move(error)};
						return false;
					}
					ids.IssuedNodeIds.insert(state.Ids.IssuedNodeIds.begin(), state.Ids.IssuedNodeIds.end());
					ids.IssuedGroupIds.insert(
						state.Ids.IssuedGroupIds.begin(), state.Ids.IssuedGroupIds.end()
					);
					ids.NextNodeId = std::max(ids.NextNodeId, state.Ids.NextNodeId);
					ids.NextGroupId = std::max(ids.NextGroupId, state.Ids.NextGroupId);
					while (ids.IssuedNodeIds.contains("node-" + std::to_string(ids.NextNodeId)))
						++ids.NextNodeId;
					while (ids.IssuedGroupIds.contains("group-" + std::to_string(ids.NextGroupId)))
						++ids.NextGroupId;
					if (!state.Canvas.Selection().empty()) {
						const auto old = state.Ids.ToDocument.find(state.Canvas.Selection().front());
						if (old != state.Ids.ToDocument.end()) {
							const auto selected = ids.ToCanvas.find(old->second);
							if (selected != ids.ToCanvas.end()) selection.push_back(selected->second);
						}
					}
					output = state.SelectedOutput;
					if (std::none_of(restored.Outputs.begin(), restored.Outputs.end(), [&](const auto &row) {
							return row.Id == output;
						}))
						output = restored.Outputs.empty() ? std::string{} : restored.Outputs.front().Id;
					if (!charge(detail::ImageGraphHistoryCanvasBytes(graph, ids)) ||
						!charge(selection.capacity() * sizeof(nodegraph::NodeId)) ||
						!charge(output.capacity()))
						return false;
					// grug cache observer accounts borrowed owners itself after all staging finishes.
					remaining += borrowed;
					return true;
				};
			if (!detail::ApplyImageGraphCacheHistory(
					state.Authored,
					state.History,
					state.FeedbackHost,
					state.CacheEditObservation,
					state.Playback,
					redo,
					state.LastDiagnostic,
					prepare
				))
				return;
			static_assert(std::is_nothrow_move_assignable_v<nodegraph::Graph>);
			static_assert(std::is_nothrow_move_assignable_v<ImageGraphCanvasIds>);
			static_assert(std::is_nothrow_move_assignable_v<decltype(state.ImportedPxcx)>);
			state.Graph = std::move(graph);
			state.Ids = std::move(ids);
			state.Canvas.Select(std::move(selection));
			state.SelectedOutput = std::move(output);
			state.CanvasNeedsReload = false;
			state.AdapterError.clear();
			if (source.Changed) {
				state.ImportedPxcx = std::move(source.Archive);
				state.PxcxProjection = std::move(source.Projection);
				state.PxcxDiagnostics = std::move(source.Diagnostics);
				state.PxcxReferenceThumbnail = std::move(source.Thumbnail);
				state.RetiredPxcxThumbnailTexture = state.CurrentPxcxThumbnailTexture;
				state.CurrentPxcxThumbnailTexture = {};
				state.PxcxThumbnailTextureHash = 0;
				state.PxcxThumbnailMessage.clear();
			}
			if (state.History.CurrentCollections())
				state.CollectionManagers = state.History.CurrentCollections();
			state.PublishedPxcx = {};
			state.GroupHost.Clear();
			state.PendingGroupRender.clear();
			state.CacheEditBlocked = false;
			state.CacheEditRetryKind = detail::ImageGraphCacheEditKind::AnimatorUndo;
			PublishAuthoredDocumentChanged(state);
			RequestPreview(state, true);
		} catch (const std::bad_alloc &) {
			state.LastDiagnostic = {
				Status::LimitExceeded, {}, {}, "Studio history staging allocation refused"
			};
		}

		bool ApplyDocumentEdit(State &state, const auto &edit, bool *unchanged = nullptr) {
			if (!RetryCacheEdit(state)) return false;
			const bool accepted = ApplyImageGraphDocumentEdit(
				state.Authored,
				state.History,
				[&](Document &document) {
					if constexpr (std::is_same_v<std::invoke_result_t<decltype(edit), Document &>, bool>) {
						if (!edit(document)) return false;
					} else
						edit(document);
					return ReconcileSplitOutputs(state, document);
				},
				unchanged
			);
			if (accepted)
				AuthoredDocumentChanged(state);
			else if (state.GroupHost.Revision != state.DocumentRevision)
				// A refused staged transaction must rebuild previews from the retained
				// document.
				state.GroupHost.Clear();
			return accepted;
		}

		bool
		ApplyKeyEdit(State &state, const auto &edit, bool *unchanged = nullptr, uint64_t borrowedBytes = 0) {
			struct RestoreDrawBudget {
				TimelineKeyEditor &Editor;
				uint64_t Bytes;
				~RestoreDrawBudget() {
					Editor.BorrowedBytes = Bytes;
				}
			} restoreDrawBudget{state.Keys, state.Keys.BorrowedBytes};
			state.Keys.BorrowedBytes = 0;
			if (!RetryCacheEdit(state)) return false;
			CancelComposerPreview(state);
			engine::imagegraph::EvaluationRequest request;
			request.HostProvider = &HostFor(state);
			request.AudioFrames = state.AudioFrames;
			request.AudioClips = state.AudioClips;
			engine::imagegraph::SourceFontContext fontContext;
			if (NeedsImageGraphSourceKeyCapture(state.Authored) &&
				!BindObservations(state, request, &fontContext))
				return false;
			(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
			const auto fontBytes = engine::imagegraph::SourceFontContextRetainedBytes(fontContext);
			const auto allowance =
				fontBytes && borrowedBytes <= engine::imagegraph::Limits::MaximumEvaluationBytes &&
						*fontBytes <= engine::imagegraph::Limits::MaximumEvaluationBytes - borrowedBytes
					? GroupConstructorAllowance(state, *fontBytes + borrowedBytes)
					: std::nullopt;
			if (!allowance) {
				state.LastDiagnostic = {
					Status::LimitExceeded, {}, {}, "source key edit host payload exceeds budget"
				};
				return false;
			}
			const bool accepted = ApplyImageGraphSourceKeyEdit(
				state.Authored,
				state.History,
				state.GroupHost,
				state.DocumentRevision,
				request,
				edit,
				state.LastDiagnostic,
				*allowance,
				unchanged,
				[&](Document &document,
					const engine::imagegraph::GroupReplayState *replay,
					uint64_t availableBytes) {
					return ReconcileSplitOutputs(state, document, replay, availableBytes);
				}
			);
			if (accepted) AuthoredDocumentChanged(state);
			return accepted;
		}

		bool ApplyPinnedKeyEdit(State &state, size_t index, const auto &edit) {
			if (index >= state.Authored.Keyframes.size()) return false;
			const auto &original = state.Authored.Keyframes[index];
			return ApplyKeyEdit(state, [&](Document &document, uint64_t availableBytes) {
				return EditImageGraphPinnedKey(
					document, state.Authored, original, availableBytes, edit, state.LastDiagnostic
				);
			});
		}

		engine::imagegraph::TimelineSettings PlaybackTimeline(const ImageGraphPlayback &playback) {
			return {
				playback.TotalFrames,
				playback.StartTick,
				playback.EndTick,
				playback.PingPong ? "pingpong"
				: playback.Loop	  ? "loop"
								  : "stop",
				playback.FramesPerSecond,
				playback.SourceBounds
			};
		}

		void ApplyPlaybackTimelineProjection(
			ImageGraphPlayback &playback, const engine::imagegraph::TimelineSettings &timeline
		) {
			playback.TotalFrames = timeline.Frames;
			playback.StartTick = timeline.First;
			playback.EndTick = timeline.Last;
			playback.Loop = timeline.Playback == "loop";
			playback.PingPong = timeline.Playback == "pingpong";
			playback.FramesPerSecond = timeline.FramesPerSecond;
			playback.SourceBounds = timeline.SourceBounds;
			if (playback.NegativeFrame || playback.CurrentTick < playback.StartTick ||
				playback.CurrentTick > playback.EndTick) {
				(void)SetImageGraphAuthorFrame(
					playback,
					{std::clamp(playback.CurrentTick, playback.StartTick, playback.EndTick), 0.0, false}
				);
			}
		}

		bool CommitPlaybackTimeline(State &state, engine::imagegraph::TimelineSettings timeline) {
			if (engine::imagegraph::ProjectSourceTimelineWindow(timeline, state.LastDiagnostic) !=
				engine::imagegraph::Status::Ok)
				return false;
			const bool accepted = ApplyDocumentEdit(state, [&](Document &document) {
				return studio::SetImageGraphTimeline(document, timeline, state.LastDiagnostic);
			});
			if (!accepted) return false;
			ApplyPlaybackTimelineProjection(state.Playback, timeline);
			state.LastDiagnostic = {};
			return true;
		}

		bool CommitSourcePlaybackTransition(State &state, const auto &transition) {
			if (!RetryCacheEdit(state)) return false;
			Diagnostic diagnostic;
			detail::PreparedSourceTimelineStep prepared;
			if (!detail::PrepareSourceTimelineStep(state.Authored, state.Playback, prepared, diagnostic)) {
				state.LastDiagnostic = std::move(diagnostic);
				return false;
			}
			if (!transition(prepared.Playback)) return false;
			bool documentChanged = false;
			if (!detail::CommitSourceTimelineStep(
					state.Authored,
					state.History,
					state.Playback,
					std::move(prepared),
					documentChanged,
					diagnostic
				)) {
				state.LastDiagnostic = std::move(diagnostic);
				return false;
			}
			if (documentChanged) AuthoredDocumentChanged(state, detail::ImageGraphCacheEditKind::RenderOnly);
			if (documentChanged) RequestPreview(state, true);
			return true;
		}

		bool SeekSourceBound(State &state, bool first) {
			const auto frame = first ? detail::SelectedRegionFirstFrame(state.Playback)
									 : detail::SelectedRegionLastFrame(state.Playback);
			engine::imagegraph::FrameTime target;
			if (!frame || !engine::imagegraph::SplitFrameTime(*frame, target, false)) {
				state.LastDiagnostic = {
					Status::InvalidValue,
					{},
					"timeline",
					"source frame endpoint exceeds the native authoring clock"
				};
				return false;
			}
			if (!SetImageGraphAuthorFrame(state.Playback, target)) return false;
			RequestPreview(state);
			return true;
		}

		bool SavePlaybackTimeline(State &state) {
			const bool saved = CommitPlaybackTimeline(state, PlaybackTimeline(state.Playback));
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
			if (removed) state.Playback.SourceBounds.reset();
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
					ClearFeedbackHost(state);
					state.SourceCommonInitialState = engine::imagegraph::SourceNodeInitialState::Constructed;
					state.ExportGrants.clear();
					state.FileGrants.clear();
					state.ImageCacheLayouts.clear();
					state.DirectoryGrants.clear();
					state.FileControls.clear();
					state.ExportUpdate = {};
					AuthoredDocumentChanged(state, detail::ImageGraphCacheEditKind::FreshDocument);
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
			ImGui::SameLine();
			detail::DrawImageComposerView(state.Panels);
			if (state.ComposerCadence.Displayed &&
				*state.ComposerCadence.Displayed != GetImageGraphFrame(state.Playback)) {
				ImGui::SameLine();
				ImGui::Text(
					"preview %.20Lg", engine::imagegraph::FrameTimeToReal(*state.ComposerCadence.Displayed)
				);
			}
		}

		void DrawPalette(State &state) {
			ImGui::InputTextWithHint(
				"##image-node-search", "Search nodes", state.Search, sizeof(state.Search)
			);
			ImGui::Separator();
			const std::string_view search(state.Search);
			const auto contains = [](std::string_view text, std::string_view query) {
				if (query.empty()) return true;
				if (query.size() > text.size()) return false;
				for (size_t start = 0; start <= text.size() - query.size(); ++start) {
					size_t offset = 0;
					while (offset < query.size() &&
						   std::tolower(static_cast<unsigned char>(text[start + offset])) ==
							   std::tolower(static_cast<unsigned char>(query[offset]))) {
						++offset;
					}
					if (offset == query.size()) return true;
				}
				return false;
			};
			struct PaletteCategory {
				std::string_view Name;
				std::vector<const nodegraph::NodeType *> Types;
			};
			std::vector<PaletteCategory> categories;
			std::unordered_map<std::string_view, size_t> categoryIndexes;
			for (const nodegraph::NodeType &type : nodegraph::NodeTypes::All()) {
				if (engine::imagegraph::FindSchema(type.Id) == nullptr) continue;
				auto [found, inserted] = categoryIndexes.try_emplace(type.Category, categories.size());
				if (inserted) categories.push_back({type.Category, {}});
				categories[found->second].Types.push_back(&type);
			}

			for (const PaletteCategory &category : categories) {
				bool hasMatch = false;
				for (const nodegraph::NodeType *type : category.Types) {
					if (contains(type->Title, search) || contains(type->Id, search) ||
						contains(category.Name, search)) {
						hasMatch = true;
						break;
					}
				}
				if (!hasMatch) continue;

				// Keep the whole catalogue browsable, and reveal every category that
				// contains a search hit so filtering never hides its own results.
				ImGui::SetNextItemOpen(true, search.empty() ? ImGuiCond_Once : ImGuiCond_Always);
				if (!ImGui::CollapsingHeader(std::string(category.Name).c_str())) continue;

				for (const nodegraph::NodeType *type : category.Types) {
					if (!contains(type->Title, search) && !contains(type->Id, search) &&
						!contains(category.Name, search))
						continue;
					ImGui::PushID(type->Id.c_str());
					if (ImGui::Selectable(type->Title.c_str())) {
						const float offset = static_cast<float>(state.Graph.Nodes().size()) * 40.0f;
						const nodegraph::NodeId node =
							state.Graph.Add(type->Id, 40.0f + offset, 40.0f + offset * 0.2f);
						if (node != nodegraph::NO_NODE) {
							state.Canvas.Select(node);
							state.Canvas.Centre(state.Graph, node);
							SyncCanvas(state);
						}
					}
					ImGui::PopID();
				}
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

		std::string_view SourceCollectionGroupId(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::SourceCommonOwnerRecord &owner
		) {
			if (owner.NativeOwnerKind == engine::imagegraph::SourceCommonNativeOwnerKind::Group)
				return owner.NativeOwnerId;
			const auto group =
				std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &candidate) {
					return candidate.OwnerNodeId == owner.NativeOwnerId;
				});
			return group == document.Groups.end() ? std::string_view{} : std::string_view(group->Id);
		}

		std::vector<std::string>
		SourceCollectionMembers(const engine::imagegraph::Document &document, std::string_view groupId) {
			std::vector<std::string> members;
			for (const auto &node : document.Nodes)
				if (node.GroupId == groupId) members.push_back(node.Id);
			for (const auto &group : document.Groups)
				if (group.ParentId == groupId) members.push_back(group.Id);
			return members;
		}

		bool IsSourceCollectionOwner(
			const engine::imagegraph::Document &document,
			const engine::imagegraph::SourceCommonOwnerRecord &owner
		) {
			using namespace engine::imagegraph;
			if (!owner.Active) return false;
			if (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group) {
				if (owner.SourceType != "Node_Group" && owner.SourceType != "Node_Collection") return false;
				const bool groupExists =
					std::any_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
						return group.Id == owner.NativeOwnerId;
					});
				// These are the exact native group dispatch identities accepted by
				// SourceCommonOwnerDispatch.
				return groupExists && SourceCommonDispatchProfile(owner.SourceType, true).Step ==
										  SourceCommonStepKind::CollectionOverride;
			}
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
					return candidate.Id == owner.NativeOwnerId;
				});
			if (node == document.Nodes.end()) return false;
			const auto *entry = FindCatalogueEntry(node->Type);
			// Core keeps CollectionOverride Step when its callback wrapper is unsupported.
			return entry && entry->SourceNode == owner.SourceType &&
				   SourceCommonDispatchProfile(owner.SourceType, true).Step ==
					   SourceCommonStepKind::CollectionOverride;
		}

		void ReconcileSourceCollections(State &state) {
			auto &collections = state.SourceCollections;
			for (auto &collection : collections) {
				const auto owner = std::find_if(
					state.Authored.SourceCommonOwners.begin(),
					state.Authored.SourceCommonOwners.end(),
					[&](const auto &candidate) {
						return candidate.SourceOwnerId == collection.OwnerId &&
							   IsSourceCollectionOwner(state.Authored, candidate);
					}
				);
				if (owner == state.Authored.SourceCommonOwners.end()) continue;
				collection.SourceType = owner->SourceType;
				const std::string_view groupId = SourceCollectionGroupId(state.Authored, *owner);
				if (groupId.empty()) {
					collection.OwnerId.clear();
					continue;
				}
				auto members = SourceCollectionMembers(state.Authored, groupId);
				if (collection.GroupId != groupId || collection.Members != members) {
					collection.RefreshNodesPending = true;
					collection.RefreshNodeDisplayPending = true;
					collection.GroupId = groupId;
					collection.Members = std::move(members);
				}
			}
			collections.erase(
				std::remove_if(
					collections.begin(),
					collections.end(),
					[&](const auto &collection) {
						return collection.OwnerId.empty() ||
							   std::none_of(
								   state.Authored.SourceCommonOwners.begin(),
								   state.Authored.SourceCommonOwners.end(),
								   [&](const auto &owner) {
									   return owner.SourceOwnerId == collection.OwnerId &&
											  IsSourceCollectionOwner(state.Authored, owner);
								   }
							   );
					}
				),
				collections.end()
			);
			for (const auto &owner : state.Authored.SourceCommonOwners) {
				if (!IsSourceCollectionOwner(state.Authored, owner)) continue;
				if (std::any_of(collections.begin(), collections.end(), [&](const auto &item) {
						return item.OwnerId == owner.SourceOwnerId;
					}))
					continue;
				const std::string_view groupId = SourceCollectionGroupId(state.Authored, owner);
				if (groupId.empty()) continue;
				collections.push_back(
					{owner.SourceOwnerId,
					 owner.SourceType,
					 std::string(groupId),
					 SourceCollectionMembers(state.Authored, groupId),
					 false,
					 false}
				);
			}
		}

		bool AdvanceSourceCommonRuntime(
			State &state,
			engine::imagegraph::EvaluationRequest &request,
			engine::imagegraph::Diagnostic &diagnostic
		) try {
			using namespace engine::imagegraph;
			const bool hasCommonOwners = !state.Authored.SourceCommonOwners.empty();
			if (!hasCommonOwners && !state.SourceCommonRuntimeInitialized &&
				state.SourceCommonOwnerSnapshot.empty())
				return true;
			if (!hasCommonOwners && state.SourceCommonRuntimeInitialized &&
				state.SourceCommonRuntimeRevision == state.DocumentRevision &&
				state.SourceCommonOwnerSnapshot.empty())
				return true;
			Plan plan;
			const auto compile = hasCommonOwners || state.Authored.Outputs.empty()
									 ? CompileSourceCommonRuntime(state.Authored, plan, diagnostic)
									 : Compile(state.Authored, plan, diagnostic);
			if (compile != Status::Ok) return false;
			request.GroupAuthoringRevision = state.DocumentRevision;
			if (!state.SourceCommonRuntimeInitialized && hasCommonOwners) {
				const auto status = state.FeedbackHost.InitializeSourceCommonRuntime(
					state.Authored,
					plan,
					request,
					state.SourceCommonInitialState,
					diagnostic,
					state.DocumentRevision,
					state.EvaluationInputRevision
				);
				if (status != Status::Ok) return false;
				state.SourceCommonRuntimeInitialized = true;
				state.SourceCommonRuntimeRevision = state.DocumentRevision;
				state.SourceCommonOwnerSnapshot = state.Authored.SourceCommonOwners;
				ReconcileSourceCollections(state);
			} else if (state.SourceCommonRuntimeRevision != state.DocumentRevision || !hasCommonOwners) {
				std::vector<SourceCommonWriterIdentity> resetWriters;
				const auto appendWriter = [&](std::string_view writer, std::string_view port) {
					if (writer.empty() || port.empty()) return;
					if (std::none_of(resetWriters.begin(), resetWriters.end(), [&](const auto &item) {
							return item.OwnerId == writer && item.Port == port;
						}))
						resetWriters.push_back({writer, port});
				};
				for (const auto &owner : state.Authored.SourceCommonOwners) {
					const auto previous = std::find_if(
						state.SourceCommonOwnerSnapshot.begin(),
						state.SourceCommonOwnerSnapshot.end(),
						[&](const auto &candidate) { return candidate.SourceOwnerId == owner.SourceOwnerId; }
					);
					if (previous == state.SourceCommonOwnerSnapshot.end() ||
						(previous->UpdateExpression == owner.UpdateExpression &&
						 previous->UpdateOverrideInstance == owner.UpdateOverrideInstance &&
						 previous->UpdateAnimatorOwnerId == owner.UpdateAnimatorOwnerId &&
						 previous->UpdateAnimatorPort == owner.UpdateAnimatorPort))
						continue;
					appendWriter(previous->UpdateAnimatorOwnerId, previous->UpdateAnimatorPort);
					appendWriter(owner.UpdateAnimatorOwnerId, owner.UpdateAnimatorPort);
				}
				for (const auto &previous : state.SourceCommonOwnerSnapshot) {
					const auto current = std::find_if(
						state.Authored.SourceCommonOwners.begin(),
						state.Authored.SourceCommonOwners.end(),
						[&](const auto &owner) { return owner.SourceOwnerId == previous.SourceOwnerId; }
					);
					if (current != state.Authored.SourceCommonOwners.end()) continue;
					appendWriter(previous.UpdateAnimatorOwnerId, previous.UpdateAnimatorPort);
				}
				const SourceCommonRuntimeReconcile operation{
					SourceNodeInitialState::Constructed, resetWriters
				};
				const auto status = state.FeedbackHost.ReconcileSourceCommonRuntime(
					state.Authored,
					plan,
					request,
					operation,
					diagnostic,
					state.DocumentRevision,
					state.EvaluationInputRevision
				);
				if (status != Status::Ok) return false;
				state.SourceCommonRuntimeInitialized = true;
				state.SourceCommonRuntimeRevision = state.DocumentRevision;
				state.SourceCommonOwnerSnapshot = state.Authored.SourceCommonOwners;
				ReconcileSourceCollections(state);
			}
			if (!hasCommonOwners) return true;

			bool refreshNodeDisplayHandled = true;
			if (std::any_of(
					state.SourceCollections.begin(), state.SourceCollections.end(), [](const auto &row) {
						return row.RefreshNodeDisplayPending;
					}
				)) {
				ReloadCanvas(state);
				refreshNodeDisplayHandled = state.AdapterError.empty();
			}
			std::vector<std::array<std::string_view, 1>> refreshGroups;
			std::vector<SourceCommonRuntimeCollectionStepObservation> collections;
			refreshGroups.reserve(state.SourceCollections.size());
			collections.reserve(state.SourceCollections.size());
			for (const auto &row : state.SourceCollections) {
				std::optional<SourcePurityRefresh> refresh;
				if (row.RefreshNodesPending) {
					refreshGroups.push_back({std::string_view(row.GroupId)});
					refresh = SourcePurityRefresh{SourcePurityRefreshEvent::Membership, refreshGroups.back()};
				}
				collections.push_back(
					{row.OwnerId,
					 row.SourceType,
					 row.RefreshNodesPending,
					 row.RefreshNodeDisplayPending,
					 row.RefreshNodeDisplayPending && refreshNodeDisplayHandled,
					 std::move(refresh)}
				);
			}
			const SourceCommonRuntimeObservations observations{{}, collections};
			const auto status = state.FeedbackHost.StepSourceCommonRuntime(
				state.Authored,
				plan,
				request,
				observations,
				diagnostic,
				state.DocumentRevision,
				state.EvaluationInputRevision
			);
			if (status != Status::Ok) return false;
			for (auto &row : state.SourceCollections)
				row.RefreshNodesPending = row.RefreshNodeDisplayPending = false;
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded,
				{},
				{},
				"Studio source common observations exceed memory bounds"
			};
			return false;
		}

		void DrawProjectSettings(State &state) {
			bool sourceSafeMode = state.SourceSafeMode;
			if (ImGui::Checkbox("Safe mode (native source host)", &sourceSafeMode)) {
				state.SourceSafeMode = sourceSafeMode;
				if (++state.EvaluationInputRevision == 0) state.EvaluationInputRevision = 1;
				RequestPreview(state, true);
			}
			ImGui::TextDisabled("Host policy only. It is not saved in the source project.");
			ImGui::Separator();
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
			// Clone the palette only when a control edits it, not on every idle panel
			// frame.
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
			bool coordinateTypeChanged = false;
			if (authored && authored->Type == "value.sample_noise" && property.Port == "position") {
				const int dimensions = std::holds_alternative<engine::imagegraph::Vector3>(replacement)	  ? 3
									   : std::holds_alternative<engine::imagegraph::Vector2>(replacement) ? 2
																										  : 1;
				const char *names[] = {"1D", "2D", "3D"};
				if (ImGui::BeginCombo("Coordinates##noise", names[dimensions - 1])) {
					for (int dimension = 1; dimension <= 3; ++dimension)
						if (ImGui::Selectable(names[dimension - 1], dimension == dimensions)) {
							if (dimension == 1)
								replacement = 0.0;
							else if (dimension == 2)
								replacement = engine::imagegraph::Vector2{};
							else
								replacement = engine::imagegraph::Vector3{};
							coordinateTypeChanged = changed = true;
						}
					ImGui::EndCombo();
				}
			}
			if (coordinateTypeChanged) {
				// grug new coordinate widget starts next frame, preserving this dropdown edit.
			} else if (triggerButton) {
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
				if (node != nullptr && detail::IsImageGraphNoiseSelector(node->Type, property.Port)) {
					changed =
						detail::DrawImageGraphNoiseChoice(node->Type, property.Port, *choice).value_or(false);
				} else if (node != nullptr &&
						   detail::IsImageGraphRasterNoiseSelector(node->Type, property.Port)) {
					choice->Value = engine::imagegraph::RasterNoiseComponents(*node, &state.Authored);
					changed = detail::DrawImageGraphRasterNoiseChoice(node->Type, property.Port, *choice)
								  .value_or(false);
				} else if (node != nullptr && node->Type == "image.transform_3d" &&
						   property.Port == "projection") {
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
			// A combo selection finishes before the numeric field becomes the last ImGui
			// item.
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
					engine::imagegraph::SourceFontContext eventAtFontContext;
					if (!BindObservations(state, event.At, &eventAtFontContext)) {
						changed = false;
					} else {
						event.NodeId = std::string(nodeId);
						event.EditedPort = property.Port;
						event.LocalValue = &replacement;
						event.LocalAnimated =
							triggerButton ||
							ImageGraphGroupHost::Mode(state.Authored, *node, property.Port) ==
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
					}
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
			if (changed && authored &&
				(detail::IsImageGraphNoiseSelector(authored->Type, editedPort) ||
				 detail::IsImageGraphRasterNoiseSelector(authored->Type, editedPort)))
				ReloadCanvas(state);
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
			} else if (const auto *font = std::get_if<engine::imagegraph::FontValue>(&value)) {
				if (!font->Data) {
					ImGui::TextUnformatted("Font: empty");
				} else {
					const auto &data = *font->Data;
					ImGui::Text(
						"Font %s  %zu glyphs  %zu frames",
						data.Identity.c_str(),
						data.Glyphs.size(),
						data.Frames.size()
					);
					ImGui::Text(
						"Line height %.9g  missing advance %.9g  space advance %.9g",
						data.LineHeight,
						data.MissingAdvance,
						data.SpaceAdvance
					);
					for (size_t frame = 0; frame < data.Frames.size() && remaining; ++frame) {
						--remaining;
						const auto &image = data.Frames[frame];
						ImGui::Text(
							"Frame %zu  %u x %u  %s  %zu bytes  hash %016llx",
							frame,
							image.Width,
							image.Height,
							engine::imagegraph::DescribeSurfaceFormat(image.Format)->Name.data(),
							image.Pixels.size(),
							static_cast<unsigned long long>(image.Hash)
						);
					}
					if (data.SourceTexture && remaining) {
						--remaining;
						ImGui::Text(
							"Source texture  %u x %u  %zu bytes",
							data.SourceTexture->Width,
							data.SourceTexture->Height,
							data.SourceTexture->Pixels.size()
						);
					}
					for (const auto &measurement : data.Measurements) {
						if (!remaining) break;
						--remaining;
						ImGui::Text(
							"Text %s  %g x %g  line width %g  gap %g",
							measurement.Text.c_str(),
							measurement.Width,
							measurement.Height,
							measurement.MaximumLineWidth,
							measurement.LineGap
						);
					}
					for (const auto &glyph : data.Glyphs) {
						if (!remaining) break;
						--remaining;
						ImGui::Text(
							"U+%04X  %s  advance %g  %g x %g  frame %s",
							glyph.Character,
							glyph.Present ? "present" : "missing",
							glyph.Advance,
							glyph.Width,
							glyph.Height,
							glyph.Frame ? std::to_string(*glyph.Frame).c_str() : "none"
						);
					}
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
					   std::holds_alternative<engine::imagegraph::FontValue>(value) ||
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

		void DrawAnimationTrackControls(State &state, const Node &node, std::string_view property);

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
				const size_t originalCount = size_t(count);
				if (ImGui::InputInt("Input groups", &count)) {
					const std::string nodeId = node.Id;
					if (node.Type == "pc.hlsl" && count >= 0 && size_t(count) < originalCount) {
						CancelComposerPreview(state);
						if (detail::ApplyHlslGroupRangeRemoval(
								state.Authored,
								state.History,
								state.GroupHost,
								state.DocumentRevision,
								nodeId,
								size_t(count),
								originalCount - size_t(count),
								state.LastDiagnostic,
								[&](Document &document, ImageGraphGroupHost &host) {
									return ReconcileSplitOutputs(state, document, &host.Replay);
								}
							))
							AuthoredDocumentChanged(state);
						ReloadCanvas(state);
						return;
					}
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
					engine::imagegraph::SourceFontContext eventAtFontContext;
					if (!BindObservations(state, event.At, &eventAtFontContext)) {
						ImGui::PopID();
						return;
					}
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
				const uint64_t trackRevision = state.DocumentRevision;
				if (input.Default && FindValue(node, input.Id) == nullptr)
					DrawAnimationTrackControls(state, node, input.Id);
				if (state.DocumentRevision != trackRevision) {
					ImGui::PopID();
					return;
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(grouped || node.DynamicInputs.size() <= 1);
				const bool remove = ImGui::SmallButton("Remove");
				ImGui::EndDisabled();
				if (changed) {
					const std::string nodeId = node.Id;
					engine::imagegraphphysics::RigidProvider requestRigidProvider;
					engine::imagegraph::EvaluationRequest request;
					request.HostProvider = &HostFor(state);
					request.AudioFrames = state.AudioFrames;
					request.AudioClips = state.AudioClips;
					engine::imagegraph::SourceFontContext requestFontContext;
					bool ready = BindObservations(state, request, &requestFontContext);
					detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
					(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
					engine::imagegraph::EvaluationSnapshot directInputs;
					const engine::imagegraph::EvaluationSnapshot *preparedInputs = nullptr;
					std::optional<detail::ImageGraphComposerSynchronousScope> synchronous;
					if (ready && node.Type == "pc.hlsl" && detail::HlslRefreshControl(input.Id)) {
						CancelComposerPreview(state);
						synchronous.emplace(state.Host.Composer);
						const uint64_t revision = state.DocumentRevision;
						const uint64_t inputRevision = state.EvaluationInputRevision;
						const auto frame = GetImageGraphFrame(state.Playback);
						engine::imagegraph::Plan plan;
						ready = Compile(state.Authored, plan, state.LastDiagnostic) == Status::Ok &&
								state.GroupHost.Prepare(
									state.Authored, plan, revision, request, state.LastDiagnostic
								) &&
								state.FeedbackHost.PrepareNodeInputs(
									state.Authored,
									plan,
									revision,
									inputRevision,
									nodeId,
									request,
									state.LastDiagnostic
								);
						if (ready && state.FeedbackHost.Active())
							preparedInputs = &state.FeedbackHost.Snapshot();
						else if (ready) {
							ready =
								EvaluateNodeInputs(
									state.Authored, plan, nodeId, request, directInputs, state.LastDiagnostic
								) == Status::Ok;
							if (ready) preparedInputs = &directInputs;
						}
						if (ready && (state.DocumentRevision != revision ||
									  state.EvaluationInputRevision != inputRevision ||
									  SelectedNodeId(state) != nodeId ||
									  GetImageGraphFrame(state.Playback) != frame)) {
							state.LastDiagnostic = {
								Status::InvalidValue,
								nodeId,
								input.Id,
								"Shader edit selection or current input generation changed"
							};
							ready = false;
						}
					}
					const auto address = detail::HlslInputPort(input.Id);
					const auto *name = input.Default ? std::get_if<std::string>(&*input.Default) : nullptr;
					bool removeEmptyGroup = false;
					if (ready && node.Type == "pc.hlsl" && address && address->Field == 0 && name &&
						name->empty()) {
						const engine::imagegraph::EvaluationInputValue *resolved = nullptr;
						if (preparedInputs)
							for (const auto &value : preparedInputs->Values())
								if (value.Port == input.Id) resolved = &value;
						if (!resolved) {
							state.LastDiagnostic = {
								Status::InvalidValue,
								nodeId,
								input.Id,
								"Shader group removal requires the current resolved name input"
							};
							ready = false;
						} else
							removeEmptyGroup = !resolved->Linked;
					}
					if (removeEmptyGroup) {
						CancelComposerPreview(state);
						if (detail::ApplyHlslGroupRemoval(
								state.Authored,
								state.History,
								state.GroupHost,
								state.DocumentRevision,
								nodeId,
								address->Group,
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
					if (ready && ApplyImageGraphSourceDynamicInput(
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
									 },
									 preparedInputs
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
			detail::ImageGraphCacheEditScope editKind(
				state.CacheEditKind, detail::ImageGraphCacheEditKind::RenderOnly
			);
			if (const auto separated =
					detail::DrawImageGraphAxisControl(node, property, state.GroupHost.Replay)) {
				const std::string nodeId = node.Id, port(property);
				CancelComposerPreview(state);
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				request.AudioFrames = state.AudioFrames;
				request.AudioClips = state.AudioClips;
				engine::imagegraph::SourceFontContext fontContext;
				if (!BindObservations(state, request, &fontContext)) return;
				(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
				const auto fontBytes = engine::imagegraph::SourceFontContextRetainedBytes(fontContext);
				const auto allowance =
					fontBytes ? GroupConstructorAllowance(state, *fontBytes) : std::nullopt;
				if (!allowance) {
					state.LastDiagnostic = {
						Status::LimitExceeded, nodeId, port, "source axis edit host payload exceeds budget"
					};
					return;
				}
				engine::imagegraph::Plan axisPlan;
				if (engine::imagegraph::Compile(state.Authored, axisPlan, state.LastDiagnostic, *allowance) !=
					Status::Ok)
					return;
				const auto axisOwner = engine::imagegraph::SourceInputExpressionOwner(
					state.Authored, axisPlan, nodeId, port, &state.GroupHost.Replay
				);
				const detail::ImageGraphAxisObservationIdentity axisIdentity{
					nodeId,
					axisOwner ? std::string(*axisOwner) : std::string{},
					state.DocumentRevision,
					state.EvaluationInputRevision,
					GetImageGraphFrame(state.Playback),
					state.GroupHost.Replay.ObservationRevision()
				};
				const auto *axisObservation = state.AxisObservations.Find(axisIdentity);
				if (!*separated && !axisObservation && state.AxisReceiptFailure != Status::Ok) {
					state.LastDiagnostic = {
						state.AxisReceiptFailure,
						nodeId,
						port,
						"Source axis processing maps were unavailable within preview bounds"
					};
					return;
				}
				if (!*separated && !axisObservation &&
					state.AxisObservations.FailureFor(axisIdentity) != Status::Ok) {
					state.LastDiagnostic = {
						state.AxisObservations.FailureFor(axisIdentity),
						nodeId,
						port,
						"Source axis processing map was unavailable within preview receipt bounds"
					};
					return;
				}
				const detail::ImageGraphAxisObservation emptyAxisObservation;
				axisPlan = {};
				const auto mapsHeld = state.AxisObservations.RetainedBytes();
				const auto selectedHeld = axisObservation
											  ? axisObservation->RetainedBytes().value_or(*allowance)
											  : emptyAxisObservation.RetainedBytes().value_or(*allowance);
				const uint64_t actionMapsHeld = mapsHeld + (axisObservation ? 0 : selectedHeld);
				if (actionMapsHeld >= *allowance) {
					state.LastDiagnostic = {
						Status::LimitExceeded, nodeId, port, "Retained axis maps leave no action allowance"
					};
					return;
				}
				if (detail::ApplyImageGraphAxisControlWithObservation(
						state.GroupHost,
						state.Authored,
						state.History,
						axisObservation ? *axisObservation : emptyAxisObservation,
						axisIdentity,
						{nodeId, port, *separated, true},
						request,
						state.LastDiagnostic,
						*allowance - actionMapsHeld + selectedHeld
					)) {
					AuthoredDocumentChanged(state);
					ReloadCanvas(state);
				}
				return;
			}
			detail::DrawAnimationTrackPolicy(
				state.Authored, node, property, state.LastDiagnostic, [&](const auto &edit) {
					return ApplyKeyEdit(state, edit);
				}
			);
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
			const auto *selectedNode = FindNode(state.Authored, nodeId);
			const bool writing = selectedNode && detail::ImageGraphFileWriteType(selectedNode->Type);
			ImGui::InputTextWithHint(
				"##read-file",
				writing ? "Exact final output path" : "Exact file path",
				controls.File.data(),
				controls.File.size()
			);
			if (!writing)
				ImGui::InputTextWithHint(
					"##read-resource",
					"Resource key (blank for primary)",
					controls.Resource.data(),
					controls.Resource.size()
				);
			const auto changed = [&] {
				state.Host.RefreshFile(nodeId);
				state.PreviewCache.Clear();
				state.PreviewSequence.Invalidate();
				if (++state.EvaluationInputRevision == 0) state.EvaluationInputRevision = 1;
				RequestPreview(state, true);
			};
			if (selectedNode && selectedNode->Type == "pc.directory_search") {
				ImGui::InputTextWithHint(
					"##directory-root",
					"Exact absolute directory root",
					controls.Directory.data(),
					controls.Directory.size()
				);
				if (ImGui::Button("Grant directory")) {
					const std::filesystem::path root(controls.Directory.data());
					if (!root.is_absolute() || root.lexically_normal() != root)
						controls.Message = "Enter an exact absolute directory root.";
					else {
						auto grant = std::find_if(
							state.DirectoryGrants.begin(),
							state.DirectoryGrants.end(),
							[&](const auto &item) { return item.NodeId == nodeId; }
						);
						if (grant != state.DirectoryGrants.end()) {
							grant->Root = root;
							controls.Message.clear();
							changed();
						} else if (state.DirectoryGrants.size() == 64)
							controls.Message = "Directory grants exceed the session limit.";
						else {
							state.DirectoryGrants.push_back({std::string(nodeId), root});
							controls.Message.clear();
							changed();
						}
					}
				}
			}
			if (writing && ImGui::Button("Grant write")) {
				const std::filesystem::path file(controls.File.data());
				if (!file.is_absolute() || file.lexically_normal() != file)
					controls.Message = "Enter the exact absolute output path with its final extension.";
				else if (!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Publish)
							  .AllowsName(file.string()) ||
						 !engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle)
							  .AllowsName(file.string()))
					controls.Message = "File type is disabled by content policy.";
				else {
					auto grant =
						std::find_if(state.FileGrants.begin(), state.FileGrants.end(), [&](const auto &item) {
							return item.NodeId == nodeId && item.Resource.empty();
						});
					if (grant != state.FileGrants.end()) {
						grant->File = file;
						grant->Write = true;
						controls.Message.clear();
						changed();
					} else if (state.FileGrants.size() >= 256)
						controls.Message = "File grants exceed the session limit.";
					else {
						state.FileGrants.push_back({std::string(nodeId), file, true});
						controls.Message.clear();
						changed();
					}
				}
			}
			if (writing && selectedNode->Type == "pc.tile_tilemap_export") {
				ImGui::InputTextWithHint(
					"##room-template",
					"GameMaker room template path",
					controls.Template.data(),
					controls.Template.size()
				);
				if (ImGui::Button("Grant template")) {
					const std::filesystem::path file(controls.Template.data());
					if (!file.is_absolute() || file.lexically_normal() != file)
						controls.Message = "Enter an exact absolute template path.";
					else if (!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle)
								  .AllowsName(file.string()))
						controls.Message = "Template type is disabled by content policy.";
					else {
						auto grant = std::find_if(
							state.FileGrants.begin(), state.FileGrants.end(), [&](const auto &item) {
								return item.NodeId == nodeId && item.Resource == "tileset_gamemaker2_room.yy";
							}
						);
						if (grant != state.FileGrants.end()) {
							grant->File = file;
							grant->Write = false;
							controls.Message.clear();
							changed();
						} else if (state.FileGrants.size() >= 256)
							controls.Message = "File grants exceed the session limit.";
						else {
							state.FileGrants.push_back(
								{std::string(nodeId), file, false, "tileset_gamemaker2_room.yy"}
							);
							controls.Message.clear();
							changed();
						}
					}
				}
			}
			if (!writing && ImGui::Button("Grant read")) {
				const std::filesystem::path file(controls.File.data());
				std::string resource(controls.Resource.data());
				const auto *node = FindNode(state.Authored, nodeId);
				if (node &&
					(node->Type == "pc.image_sequence" || node->Type == "pc.image_animated" ||
					 node->Type == "pc.directory_search") &&
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
						grant->Write = false;
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
			if (ImGui::Button(writing ? "Export" : "Read / refresh")) {
				changed();
				const detail::ImageGraphExportIntent::Target target{std::string(nodeId), {}, {}, {}};
				BeginExportIntent(
					state,
					detail::ImageGraphExportIntent::Kind::HostNode,
					std::span(&target, 1),
					false,
					detail::ImageGraphExportEvent::Update
				);
			}

			if (selectedNode && detail::SourceImageType(selectedNode->Type)) {
				const auto action = [&](engine::imagegraphio::SourceImageAction kind) {
					detail::ImageGraphExportIntent::Target target{std::string(nodeId), {}, {}, {}};
					target.ImageAction = kind;
					BeginExportIntent(
						state,
						detail::ImageGraphExportIntent::Kind::HostNode,
						std::span(&target, 1),
						false,
						detail::ImageGraphExportEvent::Update
					);
				};
				detail::DrawImageGraphImageActions(
					*selectedNode,
					controls.CacheLayout,
					controls.Message,
					state.ImageCacheLayouts,
					action,
					changed
				);
			}

			if (selectedNode &&
				(selectedNode->Type == "pc.ase_file_read" || selectedNode->Type == "pc.ora_file_read" ||
				 selectedNode->Type == "pc.krita_file_read")) {
				const bool aseArtwork = selectedNode->Type == "pc.ase_file_read";
				const auto action = [&](engine::imagegraphio::SourceArtworkAction kind,
										bool matchNames = true) {
					detail::ImageGraphExportIntent::Target target{std::string(nodeId), {}, {}, {}};
					target.ArtworkAction = kind;
					target.MatchRegionNames = matchNames;
					BeginExportIntent(
						state,
						detail::ImageGraphExportIntent::Kind::HostNode,
						std::span(&target, 1),
						false,
						detail::ImageGraphExportEvent::Update
					);
				};
				if (ImGui::Button("Generate layers"))
					action(engine::imagegraphio::SourceArtworkAction::GenerateLayers);
				if (aseArtwork) {
					if (ImGui::Button("Match animation length"))
						action(engine::imagegraphio::SourceArtworkAction::MatchFrames);
					if (ImGui::Button("Import tags as regions"))
						action(
							engine::imagegraphio::SourceArtworkAction::ImportTags, !ImGui::GetIO().KeyShift
						);
				}
			}

			if (ImGui::Button(writing ? "Revoke files" : "Revoke reads")) {
				std::erase_if(state.FileGrants, [&](const auto &grant) { return grant.NodeId == nodeId; });
				std::erase_if(state.DirectoryGrants, [&](const auto &grant) {
					return grant.NodeId == nodeId;
				});
				controls.Message.clear();
				changed();
			}
			for (const auto &grant : state.DirectoryGrants)
				if (grant.NodeId == nodeId) ImGui::TextWrapped("Directory: %s", grant.Root.string().c_str());
			for (const auto &grant : state.FileGrants)
				if (grant.NodeId == nodeId)
					ImGui::TextWrapped(
						"%s: %s",
						grant.Resource.empty() ? (grant.Write ? "Output" : "Primary")
											   : grant.Resource.c_str(),
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

		void DrawCookLibraryGrants(State &state, std::string_view nodeId) {
			auto found =
				std::find_if(state.FileControls.begin(), state.FileControls.end(), [&](const auto &held) {
					return held.NodeId == nodeId;
				});
			if (found == state.FileControls.end()) {
				FileReadControls controls;
				controls.NodeId = nodeId;
				state.FileControls.push_back(std::move(controls));
				found = std::prev(state.FileControls.end());
			}
			auto &controls = *found;
			ImGui::InputTextWithHint(
				"##shader-library-name", "Library name", controls.Resource.data(), controls.Resource.size()
			);
			ImGui::InputTextWithHint(
				"##shader-library-file",
				"Exact library source path",
				controls.File.data(),
				controls.File.size()
			);
			if (ImGui::Button("Grant library")) {
				const std::filesystem::path file(controls.File.data());
				const std::string name(controls.Resource.data());
				if (name.empty() || name.size() > 1024 || !file.is_absolute() ||
					file.lexically_normal() != file)
					controls.Message = "Enter a library name and exact absolute source path.";
				else if (!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle)
							  .AllowsName(file.string()))
					controls.Message = "Library source is disabled by content policy.";
				else {
					auto grant =
						std::find_if(state.FileGrants.begin(), state.FileGrants.end(), [&](const auto &held) {
							return held.NodeId == nodeId && held.Resource == name;
						});
					if (grant != state.FileGrants.end()) {
						grant->File = file;
						grant->Write = false;
						controls.Message.clear();
					} else if (state.FileGrants.size() == 256 ||
							   std::count_if(
								   state.FileGrants.begin(), state.FileGrants.end(), [&](const auto &held) {
									   return held.NodeId == nodeId;
								   }
							   ) == 64)
						controls.Message = "Library grants exceed the session limit.";
					else {
						state.FileGrants.push_back({std::string(nodeId), file, false, name});
						controls.Message.clear();
					}
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Revoke libraries")) {
				std::erase_if(state.FileGrants, [&](const auto &held) { return held.NodeId == nodeId; });
				controls.Message.clear();
			}
			if (!controls.Message.empty()) ImGui::TextWrapped("%s", controls.Message.c_str());
		}

		void RefreshCacheGroupMarks(State &state) {
			state.CacheGroupEdit.Reconcile(state.Authored);
			std::vector<nodegraph::NodeId> marked;
			if (!state.CacheGroupEdit.OwnerId.empty()) {
				const auto owner = detail::ImageGraphCacheEditingOwner(
					state.CacheGroupEdit, state.FeedbackHost.SourceCacheGroups()
				);
				marked.reserve(owner ? owner->Members.size() + 1 : 1);
				const auto add = [&](const std::string &id) {
					const auto node = state.Ids.ToCanvas.find(id);
					if (node != state.Ids.ToCanvas.end()) marked.push_back(node->second);
				};
				add(state.CacheGroupEdit.OwnerId);
				if (owner)
					for (const auto &member : owner->Members)
						add(member);
			}
			state.Canvas.MarkNodes(std::move(marked));
		}
		void ApplyPendingCacheGroupClick(State &state) {
			if (state.CacheGroupEdit.PendingMember.empty()) return;
			std::string member = std::move(state.CacheGroupEdit.PendingMember);
			state.CacheGroupEdit.PendingMember.clear();
			FinishInactiveEdit(state);
			state.CacheGroupEdit.Reconcile(state.Authored);
			if (state.CacheGroupEdit.OwnerId.empty() || !RetryCacheEdit(state)) return;
			if (!detail::ApplyImageGraphCacheGroupMember(
					state.Authored,
					state.History,
					state.FeedbackHost,
					state.CacheEditObservation,
					state.CacheGroupEdit.OwnerId,
					member,
					state.LastDiagnostic
				))
				return;
			state.CacheEditBlocked = false;
			state.CacheEditRetryKind = detail::ImageGraphCacheEditKind::RenderOnly;
			const auto owner = state.Ids.ToCanvas.find(state.CacheGroupEdit.OwnerId);
			if (owner != state.Ids.ToCanvas.end()) state.CacheBackgrounds.Invalidate(owner->second);
			PublishAuthoredDocumentChanged(state);
			RequestPreview(state, true);
		}

		bool DrawSourceCommonOwnerControls(
			State &state,
			engine::imagegraph::SourceCommonNativeOwnerKind ownerKind,
			std::string_view nativeOwnerId
		) {
			using namespace engine::imagegraph;
			using engine::imagegraph::SourceCommonNativeOwnerKind;
			const auto owner = std::find_if(
				state.Authored.SourceCommonOwners.begin(),
				state.Authored.SourceCommonOwners.end(),
				[&](const auto &candidate) {
					return candidate.NativeOwnerKind == ownerKind && candidate.NativeOwnerId == nativeOwnerId;
				}
			);
			if (owner == state.Authored.SourceCommonOwners.end()) return false;
			ImGui::Separator();
			ImGui::TextUnformatted("Source common sockets");
			bool showUpdate = owner->ShowUpdateTrigger;
			bool outputMetadata = owner->OutMeta;
			const bool updateChanged = ImGui::Checkbox("Show Update trigger", &showUpdate);
			const bool metadataChanged = ImGui::Checkbox("Show Updated and metadata", &outputMetadata);
			const bool changed = updateChanged || metadataChanged;
			const std::string sourceOwnerId = owner->SourceOwnerId;
			if (changed) {
				const bool accepted = ApplyDocumentEdit(state, [&](Document &document) {
					const auto target = std::find_if(
						document.SourceCommonOwners.begin(),
						document.SourceCommonOwners.end(),
						[&](const auto &candidate) { return candidate.SourceOwnerId == sourceOwnerId; }
					);
					if (target != document.SourceCommonOwners.end()) {
						target->ShowUpdateTrigger = showUpdate;
						target->OutMeta = outputMetadata;
					}
				});
				if (accepted) ReloadCanvas(state);
				return accepted;
			}
			Plan plan;
			Diagnostic planDiagnostic;
			if (CompileSourceCommonRuntime(state.Authored, plan, planDiagnostic) != Status::Ok) {
				ImGui::TextDisabled("Common getter state is unavailable.");
				return false;
			}
			EvaluationRequest request;
			request.SourceSafeMode = state.SourceSafeMode;
			(void)SetFrameTime(request, GetImageGraphFrame(state.Playback));
			const auto showValue = [&](SourceCommonSelector selector, const char *label) {
				EvaluatedValue value;
				Diagnostic diagnostic;
				const auto status = state.FeedbackHost.ReadSourceCommonGetter(
					state.Authored,
					plan,
					sourceOwnerId,
					selector,
					request,
					value,
					diagnostic,
					state.DocumentRevision,
					state.EvaluationInputRevision
				);
				if (status != Status::Ok) {
					ImGui::TextDisabled("%s: unavailable", label);
				} else if (const auto *boolean = std::get_if<bool>(&value.Data)) {
					ImGui::Text("%s: %s", label, *boolean ? "true" : "false");
				} else if (const auto *text = std::get_if<std::string>(&value.Data)) {
					ImGui::Text("%s: %s", label, text->c_str());
				} else if (const auto *position = std::get_if<Vector2>(&value.Data)) {
					ImGui::Text("%s: (%.3f, %.3f)", label, position->X, position->Y);
				} else {
					ImGui::TextDisabled("%s: held value type is not displayed", label);
				}
			};
			if (showUpdate) showValue(SourceCommonSelector::Update, "Update");
			if (outputMetadata) {
				showValue(SourceCommonSelector::Updated, "Updated");
				showValue(SourceCommonSelector::Name, "Name");
				showValue(SourceCommonSelector::Position, "Position");
			}
			return false;
		}

		void DrawInspector(
			State &state,
			engine::render::Renderer &renderer,
			engine::core::Name owner,
			const engine::assets::LocalPaths &paths
		) {
			const std::string nodeId = SelectedNodeId(state);
			Node *node = FindNode(state.Authored, nodeId);
			if (node == nullptr) {
				ImGui::TextDisabled("Select one node to edit its authored values.");
				return;
			}
			ImGui::TextUnformatted(node->Type.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s", node->Id.c_str());
			if (DrawSourceCommonOwnerControls(
					state, engine::imagegraph::SourceCommonNativeOwnerKind::Node, node->Id
				))
				return;
			const engine::imagegraph::NodeSchema *schema = engine::imagegraph::FindSchema(node->Type);
			if (schema == nullptr) {
				ImGui::TextDisabled("This node type is not registered in this build.");
				return;
			}
			if (node->Type == "pc.cache" || node->Type == "pc.cache_array") {
				const auto serialize = detail::DrawImageGraphCacheControls(
					*node, state.CacheGroupEdit, state.FeedbackHost.SourceCacheGroups()
				);
				if (serialize) {
					FinishInactiveEdit(state);
					if (RetryCacheEdit(state) && detail::ApplyImageGraphCacheSerialize(
													 state.Authored,
													 state.History,
													 state.FeedbackHost,
													 state.CacheEditObservation,
													 nodeId,
													 *serialize,
													 state.LastDiagnostic
												 )) {
						state.CacheEditBlocked = false;
						state.CacheEditRetryKind = detail::ImageGraphCacheEditKind::RenderOnly;
						PublishAuthoredDocumentChanged(state);
						RequestPreview(state, true);
					}
					node = FindNode(state.Authored, nodeId);
					if (!node) return;
					schema = engine::imagegraph::FindSchema(node->Type);
					if (!schema) return;
				}
			}
			if (node->Type == "pc.rigid_object") {
				engine::imagegraph::EvaluationSnapshot directInputs;
				detail::DrawImageGraphRigidMeshAction(
					state.Authored,
					state.History,
					nodeId,
					[&](Diagnostic &error) -> const engine::imagegraph::EvaluationSnapshot * {
						FinishInactiveEdit(state);
						const auto *target = FindNode(state.Authored, nodeId);
						if (SelectedNodeId(state) != nodeId || !target || target->Type != "pc.rigid_object") {
							error = {
								Status::InvalidValue,
								nodeId,
								"attribute_mesh",
								"Generate Mesh selection changed"
							};
							return nullptr;
						}
						const uint64_t revision = state.DocumentRevision;
						const uint64_t inputRevision = state.EvaluationInputRevision;
						const auto current = [&] {
							return detail::ImageGraphRigidMeshCaptureCurrent(
								state.Authored,
								nodeId,
								SelectedNodeId(state),
								state.DocumentRevision,
								state.EvaluationInputRevision,
								revision,
								inputRevision,
								error
							);
						};
						engine::imagegraphphysics::RigidProvider requestRigidProvider;
						engine::imagegraph::EvaluationRequest request;
						request.HostProvider = &HostFor(state);
						engine::imagegraph::SourceFontContext requestFontContext;
						if (!BindObservations(state, request, &requestFontContext)) return nullptr;
						detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
						(void)SetFrameTime(request, GetImageGraphFrame(state.Playback));
						request.AudioFrames = state.AudioFrames;
						request.AudioClips = state.AudioClips;
						request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
						engine::imagegraph::Plan plan;
						if (Compile(state.Authored, plan, error) != Status::Ok ||
							!state.GroupHost.Prepare(
								state.Authored, plan, state.DocumentRevision, request, error
							) ||
							!state.FeedbackHost.PrepareNodeInputs(
								state.Authored,
								plan,
								state.DocumentRevision,
								state.EvaluationInputRevision,
								nodeId,
								request,
								error
							))
							return nullptr;
						if (!current()) return nullptr;
						if (state.FeedbackHost.Active()) return &state.FeedbackHost.Snapshot();
						if (EvaluateNodeInputs(state.Authored, plan, nodeId, request, directInputs, error) !=
							Status::Ok)
							return nullptr;
						return current() ? &directInputs : nullptr;
					},
					[&] {
						AuthoredDocumentChanged(state);
						state.PreviewCache.Clear();
						state.PreviewSequence.Invalidate();
						RequestPreview(state, true);
					},
					state.LastDiagnostic
				);
				node = FindNode(state.Authored, nodeId);
				if (!node) return;
				schema = engine::imagegraph::FindSchema(node->Type);
				if (!schema) return;
			}
			if (node->Type == "pc.hlsl") DrawCookLibraryGrants(state, nodeId);
			if (node->Type == "pc.hlsl" && ImGui::Button("Cook Shader")) {
				CancelComposerPreview(state);
				const detail::ImageGraphComposerSynchronousScope synchronous(state.Host.Composer);
				engine::imagegraph::EvaluationSnapshot directInputs;
				detail::ImageGraphCookCapture captured;
				const auto capture =
					[&](Diagnostic &error) -> const engine::imagegraph::EvaluationSnapshot * {
					FinishInactiveEdit(state);
					const auto *target = FindNode(state.Authored, nodeId);
					if (SelectedNodeId(state) != nodeId || !target || target->Type != "pc.hlsl") {
						error = {
							Status::InvalidValue,
							nodeId,
							"composer_cooked_shader",
							"Cook Shader selection changed"
						};
						return nullptr;
					}
					const uint64_t revision = state.DocumentRevision;
					const uint64_t inputRevision = state.EvaluationInputRevision;
					captured = {nodeId, owner, revision, inputRevision};
					const auto current = [&] {
						if (detail::ImageGraphCookCaptureCurrent(
								state.Authored,
								captured,
								SelectedNodeId(state),
								owner,
								state.DocumentRevision,
								state.EvaluationInputRevision
							))
							return true;
						error = {
							Status::InvalidValue,
							nodeId,
							"composer_cooked_shader",
							"Cook selection, owner or inputs changed during capture"
						};
						return false;
					};
					engine::imagegraphphysics::RigidProvider requestRigidProvider;
					engine::imagegraph::EvaluationRequest request;
					request.HostProvider = &HostFor(state);
					engine::imagegraph::SourceFontContext requestFontContext;
					if (!BindObservations(state, request, &requestFontContext)) return nullptr;
					detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
					(void)SetFrameTime(request, GetImageGraphFrame(state.Playback));
					request.AudioFrames = state.AudioFrames;
					request.AudioClips = state.AudioClips;
					request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
					engine::imagegraph::Plan plan;
					if (Compile(state.Authored, plan, error) != Status::Ok ||
						!state.GroupHost.Prepare(
							state.Authored, plan, state.DocumentRevision, request, error
						) ||
						!state.FeedbackHost.PrepareNodeInputs(
							state.Authored,
							plan,
							state.DocumentRevision,
							state.EvaluationInputRevision,
							nodeId,
							request,
							error
						))
						return nullptr;
					if (!current()) return nullptr;
					if (state.FeedbackHost.Active()) return &state.FeedbackHost.Snapshot();
					if (EvaluateNodeInputs(state.Authored, plan, nodeId, request, directInputs, error) !=
						Status::Ok)
						return nullptr;
					return current() ? &directInputs : nullptr;
				};
				const auto *inputs = capture(state.LastDiagnostic);
				detail::ImageGraphCookLibraries libraries;
				std::string libraryFailure;
				const auto documentBytes = DocumentRetainedPayloadBytes(state.Authored);
				const uint64_t maximumCookBytes = engine::imagegraph::Limits::MaximumEvaluationBytes;
				const bool captureAdmitted = inputs && documentBytes &&
											 *documentBytes <= maximumCookBytes / 2 &&
											 inputs->RetainedBytes() <= maximumCookBytes - 2 * *documentBytes;
				if (inputs && !captureAdmitted)
					libraryFailure = "Cook capture exceeds the operation byte budget";
				const bool librariesRead =
					captureAdmitted && detail::ReadImageGraphCookLibraries(
										   state.FileGrants,
										   nodeId,
										   libraries,
										   libraryFailure,
										   maximumCookBytes - 2 * *documentBytes - inputs->RetainedBytes()
									   );
				if (inputs && !librariesRead)
					state.LastDiagnostic = {Status::InvalidValue, nodeId, "libraries", libraryFailure};
				if (librariesRead &&
					detail::ApplyImageGraphCookAction(
						state.Authored,
						state.History,
						captured,
						SelectedNodeId(state),
						owner,
						state.DocumentRevision,
						state.EvaluationInputRevision,
						*inputs,
						paths,
						libraries.Views,
						engine::render::hlsl::CookerIdentity(),
						[&](auto world, auto asset) { return renderer.ComposerShaderRevision(world, asset); },
						[&](auto world, auto asset, const auto &artifact) {
							return renderer.InstallComposerShader(world, asset, artifact);
						},
						[&](auto world, auto asset, auto revision) {
							return renderer.RemoveComposerShader(world, asset, revision);
						},
						state.LastDiagnostic,
						engine::imagegraph::Limits::MaximumEvaluationBytes - libraries.Bytes
					)) {
					AuthoredDocumentChanged(state);
					state.PreviewCache.Clear();
					state.PreviewSequence.Invalidate();
					RequestPreview(state, true);
				}
				node = FindNode(state.Authored, nodeId);
				if (!node) return;
				schema = engine::imagegraph::FindSchema(node->Type);
				if (!schema) return;
			}
			if (node->Type == "pc.cache_results" && ImGui::Button("Clear cache")) {
				engine::imagegraph::Plan plan;
				if (engine::imagegraph::Compile(state.Authored, plan, state.LastDiagnostic) == Status::Ok &&
					detail::ApplyImageGraphCacheResultsClear(
						state.Authored,
						plan,
						state.FeedbackHost,
						state.PreviewCache,
						nodeId,
						SelectedNodeId(state),
						state.DocumentRevision,
						state.EvaluationInputRevision,
						state.LastDiagnostic
					)) {
					state.PreviewSequence.Invalidate();
					RequestPreview(state, true);
				}
			}
			if (node->Type == "pc.verlet_sim_mesh_cache" && ImGui::Button("Cache Mesh")) {
				engine::imagegraphphysics::RigidProvider requestRigidProvider;
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				engine::imagegraph::SourceFontContext requestFontContext;
				if (!BindObservations(state, request, &requestFontContext)) return;
				detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
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
			if ((detail::ImageGraphFileReadType(node->Type) || detail::ImageGraphFileWriteType(node->Type)) &&
				!detail::ImageGraphFileUsesOwnedContent(node->Type))
				DrawFileGrants(state, nodeId);
			if (schema->Properties.empty() && !schema->DynamicInputs) {
				ImGui::TextDisabled("No authored properties.");
				return;
			}
			ImGui::Separator();
			if (node->Type == "pc.audio_window") {
				engine::imagegraphphysics::RigidProvider requestRigidProvider;
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				engine::imagegraph::SourceFontContext requestFontContext;
				engine::imagegraph::Diagnostic error;
				if (!BindObservations(state, request, &requestFontContext)) {
					ImGui::TextWrapped("%s", state.LastDiagnostic.Message.c_str());
				} else {
					detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
					(void)engine::imagegraph::SetFrameTime(request, GetImageGraphFrame(state.Playback));
					request.AudioFrames = state.AudioFrames;
					request.AudioClips = state.AudioClips;
					request.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
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
			}
			if (node->Type == "pc.wav_file_read") {
				if (ImGui::Button("Sync length")) {
					engine::imagegraphphysics::RigidProvider requestRigidProvider;
					engine::imagegraph::EvaluationRequest request;
					request.HostProvider = &HostFor(state);
					engine::imagegraph::SourceFontContext requestFontContext;
					if (!BindObservations(state, request, &requestFontContext)) return;
					detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
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
				bool fileChecker = true;
				Diagnostic checkerError;
				if (detail::ReadWavFileCheckerEnabled(*node, fileChecker, checkerError)) {
					if (ImGui::Checkbox("File Watcher", &fileChecker)) {
						ApplyDocumentEdit(state, [&](Document &document) {
							auto *edited = FindNode(document, nodeId);
							return edited && detail::SetWavFileCheckerEnabled(
												 *edited, fileChecker, state.LastDiagnostic
											 );
						});
						node = FindNode(state.Authored, nodeId);
						if (!node) return;
					}
					ImGui::TextWrapped("Watches only a successfully loaded WAV file.");
				} else
					ImGui::TextWrapped("%s", checkerError.Message.c_str());
				engine::imagegraphphysics::RigidProvider waveformRequestRigidProvider;
				engine::imagegraph::EvaluationRequest waveformRequest;
				waveformRequest.HostProvider = &HostFor(state);
				engine::imagegraph::SourceFontContext waveformRequestFontContext;
				engine::imagegraph::Diagnostic waveformError;
				if (!BindObservations(state, waveformRequest, &waveformRequestFontContext)) {
					ImGui::TextWrapped("%s", state.LastDiagnostic.Message.c_str());
				} else {
					detail::BindImageGraphRigid(
						waveformRequest, waveformRequestRigidProvider, state.Playback
					);
					(void)engine::imagegraph::SetFrameTime(
						waveformRequest, GetImageGraphFrame(state.Playback)
					);
					waveformRequest.AudioClips = state.AudioClips;
					waveformRequest.AudioFrames = state.AudioFrames;
					waveformRequest.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
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
			}
			if (node->Type == "pc.wav_file_write") {
				engine::imagegraphphysics::RigidProvider requestRigidProvider;
				engine::imagegraph::EvaluationRequest request;
				request.HostProvider = &HostFor(state);
				engine::imagegraph::SourceFontContext requestFontContext;
				if (!BindObservations(state, request, &requestFontContext)) {
					ImGui::TextWrapped("%s", state.LastDiagnostic.Message.c_str());
					return;
				}
				detail::BindImageGraphRigid(request, requestRigidProvider, state.Playback);
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
				if (!visibleVectorProperty(property.Port) ||
					!detail::ImageGraphNoisePropertyVisible(*node, property.Port))
					continue;
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
				if (!detail::IsImageGraphNoiseSelector(node->Type, property.Port) &&
					!detail::IsImageGraphRasterNoiseSelector(node->Type, property.Port))
					DrawAnimationTrackControls(state, *node, property.Port);
				ImGui::PopID();
				ImGui::PopID();
				if (state.DocumentRevision != revision) return;
			}
			for (const engine::imagegraph::PropertySchema &property : schema->Properties) {
				if (!visibleVectorProperty(property.Id) ||
					!detail::ImageGraphNoisePropertyVisible(*node, property.Id))
					continue;
				if (node->Type == "pc.wav_file_read" && property.Id == "sync_length") continue;
				if (FindValue(*node, property.Id) != nullptr) continue;
				if (detail::IsImageGraphRasterNoiseSelector(node->Type, property.Id)) {
					AuthoredValue choice{
						std::string(property.Id),
						engine::imagegraph::EnumValue{
							engine::imagegraph::RasterNoiseComponents(*node, &state.Authored)
						}
					};
					ImGui::PushID(node->Id.c_str());
					ImGui::PushID(choice.Port.c_str());
					ImGui::TextUnformatted("Output Type");
					ImGui::SameLine(92.0f);
					const bool changed = DrawValueWidget(state, node->Id, choice);
					ImGui::PopID();
					ImGui::PopID();
					if (changed) return;
					continue;
				}
				auto initial = ImageGraphPropertyDefault(state.Authored, node->Type, property.Id);
				if (node->Type == "value.noise_field" && property.Id == "position")
					initial = detail::ImageGraphNoisePositionDefault(*node);
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
				ClearFeedbackHost(state);
				state.PreviewCache.Clear();
				state.PreviewSequence.Invalidate();
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
				if (DrawSourceCommonOwnerControls(
						state, engine::imagegraph::SourceCommonNativeOwnerKind::Group, groupNow->Id
					))
					return;
				bool pureFunction = groupNow->PureFunction;
				if (ImGui::Checkbox("Pure function", &pureFunction)) {
					const std::string groupId = groupNow->Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						const auto group = std::find_if(
							document.Groups.begin(), document.Groups.end(), [&](const auto &candidate) {
								return candidate.Id == groupId;
							}
						);
						if (group != document.Groups.end()) {
							document.FormatVersion = std::max(document.FormatVersion, uint32_t{10});
							group->PureFunction = pureFunction;
						}
					});
					return;
				}
				bool renderActive = groupNow->RenderActive;
				if (ImGui::Checkbox("Automatic rendering", &renderActive)) {
					const std::string groupId = groupNow->Id;
					ApplyDocumentEdit(state, [&](Document &document) {
						const auto group = std::find_if(
							document.Groups.begin(), document.Groups.end(), [&](const auto &candidate) {
								return candidate.Id == groupId;
							}
						);
						if (group != document.Groups.end()) {
							document.FormatVersion = std::max(document.FormatVersion, uint32_t{10});
							group->RenderActive = renderActive;
						}
					});
					return;
				}
				ImGui::SameLine();
				if (ImGui::Button("Render group")) {
					state.PendingGroupRender = groupNow->Id;
					state.PreviewCache.Clear();
					state.PreviewSequence.Invalidate();
					RequestPreview(state, true);
				}
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
						for (auto &owner : document.SourceCommonOwners)
							if (owner.NativeOwnerKind ==
									engine::imagegraph::SourceCommonNativeOwnerKind::Group &&
								owner.NativeOwnerId == id)
								owner.DisplayNamePresent = true;
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
			ApplyKeyEdit(state, [&](Document &document, uint64_t availableBytes) {
				return SetImageGraphKeyframe(
					document,
					nodeId,
					port,
					state.Playback.CurrentTick,
					"step",
					state.LastDiagnostic,
					state.Playback.Subframe,
					state.Playback.NegativeFrame,
					availableBytes
				);
			});
		}

		void DrawKeyframeEaseSide(State &state, size_t index, const Keyframe &frame, bool incoming) {
			detail::ImageGraphCacheEditScope editKind(
				state.CacheEditKind, detail::ImageGraphCacheEditKind::RenderOnly
			);
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

			ApplyPinnedKeyEdit(
				state, index, [&](Document &document, size_t keyIndex, uint64_t availableBytes) {
					return SetImageGraphKeyframeEase(
						document, keyIndex, ease, state.LastDiagnostic, availableBytes
					);
				}
			);
		}

		void DrawKeyframeSineDriver(State &state, size_t index, const Keyframe &frame) {
			detail::ImageGraphCacheEditScope editKind(
				state.CacheEditKind, detail::ImageGraphCacheEditKind::RenderOnly
			);
			if (!std::holds_alternative<double>(frame.Data)) {
				ImGui::TextUnformatted("Scalar only");
				return;
			}
			const auto sine = frame.SineDriver;
			if (ImGui::SmallButton(sine ? "Sine..." : "Add sine")) {
				if (!sine) {
					ApplyPinnedKeyEdit(
						state, index, [&](Document &document, size_t keyIndex, uint64_t availableBytes) {
							return SetImageGraphKeyframeSineDriver(
								document,
								keyIndex,
								engine::imagegraph::KeyframeSineDriver{},
								state.LastDiagnostic,
								availableBytes
							);
						}
					);
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
				ApplyPinnedKeyEdit(
					state, index, [&](Document &document, size_t keyIndex, uint64_t availableBytes) {
						return SetImageGraphKeyframeSineDriver(
							document, keyIndex, driver, state.LastDiagnostic, availableBytes
						);
					}
				);
			}
			if (ImGui::SmallButton("Remove sine")) {
				ApplyPinnedKeyEdit(
					state, index, [&](Document &document, size_t keyIndex, uint64_t availableBytes) {
						return SetImageGraphKeyframeSineDriver(
							document, keyIndex, std::nullopt, state.LastDiagnostic, availableBytes
						);
					}
				);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		void DrawKeyframeSourceDriver(State &state, size_t index) {
			detail::ImageGraphCacheEditScope editKind(
				state.CacheEditKind, detail::ImageGraphCacheEditKind::RenderOnly
			);
			using namespace engine::imagegraph;
			const auto rowRevision = state.DocumentRevision;
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
					ApplyPinnedKeyEdit(
						state, index, [&](Document &document, size_t keyIndex, uint64_t availableBytes) {
							return SetImageGraphKeyframeSourceDriver(
								document, keyIndex, driver, state.LastDiagnostic, availableBytes
							);
						}
					);
				}
				ImGui::EndCombo();
			}
			if (rowRevision != state.DocumentRevision) {
				ImGui::EndPopup();
				return;
			}
			// Curve data is copied only while its controls are open, never for an idle
			// timeline row.
			auto driver = state.Authored.Keyframes[index].SourceDriver;
			if (driver) {
				const bool changed = detail::DrawKeyframeSourceDriverValue(*driver, [&](Curve &curve) {
					return DrawCurveValue(curve);
				});
				if (changed)
					ApplyPinnedKeyEdit(
						state, index, [&](Document &document, size_t keyIndex, uint64_t availableBytes) {
							return SetImageGraphKeyframeSourceDriver(
								document, keyIndex, driver, state.LastDiagnostic, availableBytes
							);
						}
					);
				if (rowRevision != state.DocumentRevision) {
					ImGui::EndPopup();
					return;
				}
				if (ImGui::SmallButton("Remove driver")) {
					ApplyPinnedKeyEdit(
						state, index, [&](Document &document, size_t keyIndex, uint64_t availableBytes) {
							return SetImageGraphKeyframeSourceDriver(
								document, keyIndex, std::nullopt, state.LastDiagnostic, availableBytes
							);
						}
					);
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}

		void DrawKeyframeKind(State &state, size_t index) {
			detail::ImageGraphCacheEditScope editKind(
				state.CacheEditKind, detail::ImageGraphCacheEditKind::RenderOnly
			);
			state.KeyKind.Draw(state.Authored, index, [&] {
				bool unchanged = false;
				const bool accepted = ApplyKeyEdit(
					state,
					[&](Document &document, uint64_t availableBytes) {
						return state.KeyKind.PrepareCommit(document, state.LastDiagnostic, availableBytes);
					},
					&unchanged
				);
				if (accepted || unchanged) state.KeyKind.Cancel();
				return accepted || unchanged;
			});
		}

		bool PrepareTimelineRead(State &state) {
			using namespace engine::imagegraph;
			EvaluationRequest request;
			SourceFontContext fonts;
			request.HostProvider = &HostFor(state);
			request.AudioFrames = state.AudioFrames;
			request.AudioClips = state.AudioClips;
			if (!state.TimelineRead.Matches(state.GroupHost, state.DocumentRevision) &&
				!BindObservations(state, request, &fonts))
				return false;
			const auto snapshotBytes = state.TimelineRead.RetainedBytes();
			const auto allowance = GroupConstructorAllowance(state);
			if (!snapshotBytes || !allowance ||
				*snapshotBytes > Limits::MaximumEvaluationBytes - *allowance) {
				state.LastDiagnostic = {
					Status::LimitExceeded, {}, {}, "timeline read host payload exceeds budget"
				};
				return false;
			}
			const auto fontBytes = SourceFontContextRetainedBytes(fonts);
			if (!fontBytes || *fontBytes >= *allowance) return false;
			Diagnostic error;
			if (!PrepareImageGraphTimelineRead(
					state.Authored,
					state.GroupHost,
					state.DocumentRevision,
					request,
					state.TimelineRead,
					error,
					*allowance + *snapshotBytes - *fontBytes
				)) {
				state.LastDiagnostic = std::move(error);
				return false;
			}
			const auto authorBytes = DocumentRetainedPayloadBytes(state.Authored);
			const auto readBytes = state.TimelineRead.RetainedBytes();
			const auto currentAllowance = GroupConstructorAllowance(state);
			if (!authorBytes || !readBytes || !currentAllowance) return false;
			const uint64_t held = Limits::MaximumEvaluationBytes - *currentAllowance - *readBytes;
			const uint64_t replayBytes = state.GroupHost.Replay.RetainedBytes();
			if (*authorBytes >= Limits::MaximumEvaluationBytes - held ||
				replayBytes >= Limits::MaximumEvaluationBytes - held - *authorBytes)
				return false;
			state.Keys.BorrowedBytes = held + *authorBytes + replayBytes;
			state.Keys.ObservationRevision = state.TimelineRead.DisplayRevision;
			return true;
		}
		bool TimelinePinsCurrent(State &state, uint64_t observationRevision) {
			if (state.TimelineRead.Matches(state.GroupHost, state.DocumentRevision) &&
				observationRevision == state.TimelineRead.DisplayRevision)
				return true;
			state.LastDiagnostic = {Status::InvalidValue, {}, {}, "timeline source changed while editing"};
			return false;
		}

		void DrawTimeline(State &state) {
			detail::ImageGraphCacheEditScope editKind(
				state.CacheEditKind, detail::ImageGraphCacheEditKind::RenderOnly
			);
			if (state.Regions.Draw(
					state.Authored,
					state.DocumentRevision,
					state.Playback,
					state.Dopesheet.PixelsPerFrame,
					state.Dopesheet.PanX,
					state.LastDiagnostic,
					[&](const auto &edit) { return ApplyDocumentEdit(state, edit); }
				)) {
				CancelComposerPreview(state);
				state.PreviewCache.Clear();
				state.PreviewSequence.Invalidate();
				RequestPreview(state, true);
			}

			if (ImGui::Button(state.Playback.Playing ? "Pause" : "Play")) {
				if (!state.Playback.Playing && state.Playback.SelectedRegion && state.Playback.SourceBounds)
					(void)SeekSourceBound(state, true);
				if (!state.Playback.Playing &&
					(state.Playback.NegativeFrame || state.Playback.CurrentTick < state.Playback.StartTick ||
					 state.Playback.CurrentTick > state.Playback.EndTick)) {
					(void)SeekSourceBound(state, true);
				}
				if (!state.Playback.Playing) state.Playback.Direction = 1;
				if (state.Playback.CurrentTick >= state.Playback.EndTick && !state.Playback.Loop &&
					!state.Playback.PingPong)
					(void)SeekSourceBound(state, true);
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
				RequestPreview(state, true);
			}
			ImGui::SameLine();
			ImGui::Checkbox("Simulation", &state.Playback.Simulating);
			ImGui::SameLine();
			if (ImGui::Button("Step -1") &&
				(state.Playback.CurrentTick > state.Playback.StartTick || state.Playback.Subframe > 0.0)) {
				const double target =
					double(engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback))) - 1.0;
				engine::imagegraph::FrameTime frameTarget;
				if (engine::imagegraph::SplitFrameTime(target, frameTarget, false) &&
					CommitSourcePlaybackTransition(state, [&](ImageGraphPlayback &playback) {
						const bool changed = SetImageGraphAuthorFrame(playback, frameTarget);
						playback.Direction = -1;
						return changed;
					}))
					RequestPreview(state);
			}
			ImGui::SameLine();
			if (ImGui::Button("Step +1") && state.Playback.CurrentTick < state.Playback.EndTick) {
				const double target =
					double(engine::imagegraph::FrameTimeToReal(GetImageGraphFrame(state.Playback))) + 1.0;
				engine::imagegraph::FrameTime frameTarget;
				if (engine::imagegraph::SplitFrameTime(target, frameTarget, false) &&
					CommitSourcePlaybackTransition(state, [&](ImageGraphPlayback &playback) {
						const bool changed = SetImageGraphAuthorFrame(playback, frameTarget);
						playback.Direction = 1;
						return changed;
					}))
					RequestPreview(state);
			}
			ImGui::SameLine();
			if (ImGui::Button("First frame")) (void)SeekSourceBound(state, true);
			ImGui::SameLine();
			if (ImGui::Button("Last frame")) (void)SeekSourceBound(state, false);
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
			if (state.Playback.SourceBounds) {
				auto sourceBounds = *state.Playback.SourceBounds;
				bool sourceBoundsChanged = false;
				ImGui::TextUnformatted("Source endpoints (one-based)");
				const auto drawBound = [&](const char *label,
										   const char *id,
										   engine::imagegraph::SourceAuthoringFrameBound &bound) {
					using Presence = engine::imagegraph::SourceFrameBoundPresence;
					const char *presence = bound.Presence == Presence::Missing ? "Missing"
										   : bound.Presence == Presence::Null  ? "Null"
																			   : "Explicit";
					ImGui::PushID(id);
					ImGui::TextUnformatted(label);
					ImGui::SameLine(96.0f);
					if (ImGui::BeginCombo("##presence", presence)) {
						for (const auto &[choice, value] : std::array<std::pair<const char *, Presence>, 3>{
								 {{"Missing", Presence::Missing},
								  {"Null", Presence::Null},
								  {"Explicit", Presence::Explicit}}
							 }) {
							const bool selected = bound.Presence == value;
							if (ImGui::Selectable(choice, selected)) {
								bound.Presence = value;
								if (value != Presence::Explicit) bound.Value = {};
								sourceBoundsChanged = true;
							}
							if (selected) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					if (bound.Presence == Presence::Explicit) {
						ImGui::SameLine();
						ImGui::SetNextItemWidth(120.0f);
						double authored = double(engine::imagegraph::FrameTimeToReal(bound.Value));
						if (ImGui::InputDouble("##value", &authored, 0.01, 1.0, "%.9g")) {
							engine::imagegraph::FrameTime parsed;
							if (engine::imagegraph::SplitFrameTime(
									authored, parsed, false, engine::imagegraph::Limits::MaximumTick + 1
								)) {
								bound.Value = parsed;
								sourceBoundsChanged = true;
							} else {
								state.LastDiagnostic = {
									Status::InvalidValue,
									{},
									id,
									"source endpoint exceeds the native authoring clock"
								};
							}
						}
					}
					ImGui::PopID();
				};
				drawBound("Start", "source-start", sourceBounds.Start);
				drawBound("End", "source-end", sourceBounds.End);
				if (sourceBoundsChanged) {
					auto timeline = PlaybackTimeline(state.Playback);
					timeline.SourceBounds = sourceBounds;
					(void)CommitPlaybackTimeline(state, std::move(timeline));
				}
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
					const auto rowRevision = state.DocumentRevision;
					ImGui::PushID(value.Port.c_str());
					ImGui::TextUnformatted(value.Port.c_str());
					ImGui::SameLine(96.0f);
					if (ImGui::Button("Set key")) AddKeyframe(state, node->Id, value.Port);
					if (rowRevision != state.DocumentRevision) {
						ImGui::PopID();
						break;
					}
					ImGui::SameLine();
					if (ImGui::Button("Remove at frame")) {
						ApplyKeyEdit(state, [&](Document &document, uint64_t availableBytes) {
							return RemoveImageGraphKeyframe(
								document,
								node->Id,
								value.Port,
								state.Playback.CurrentTick,
								state.LastDiagnostic,
								state.Playback.Subframe,
								state.Playback.NegativeFrame,
								availableBytes
							);
						});
					}
					ImGui::PopID();
					if (rowRevision != state.DocumentRevision) break;
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
					"Legacy cubic keys are retained and report "
					"UnsupportedExecution. Choose source for "
					"editable handles."
				);
			} else {
				ImGui::TextDisabled(
					"Source easing supports linear, Bezier and cut sides "
					"with incoming and outgoing handles."
				);
			}
			if (!state.TimelineRead.Matches(state.GroupHost, state.DocumentRevision)) return;
			const auto applyKeyTransfer = [&] {
				if (!TimelinePinsCurrent(state, state.Keys.OriginalObservationRevision)) return false;
				bool unchanged = false;
				const bool accepted = ApplyKeyEdit(
					state,
					[&](Document &document, uint64_t availableBytes) {
						if (state.Keys.Copying)
							return state.Keys.PrepareCommit(document, state.LastDiagnostic, availableBytes);
						return WithImageGraphProjectedKeyPins(
							*state.TimelineRead.Snapshot,
							document,
							state.Keys.Originals,
							availableBytes,
							[&](auto pins, uint64_t remaining) {
								return state.Keys.PrepareCommit(
									document, state.LastDiagnostic, remaining, pins
								);
							},
							state.LastDiagnostic,
							state.Keys.OriginalAxes
						);
					},
					&unchanged
				);
				if (accepted || unchanged) state.Keys.PublishCommit();
				return accepted || unchanged;
			};
			state.Dopesheet.Draw(
				*state.TimelineRead.Snapshot,
				state.TimelineRead.DisplayRevision,
				state.Keys,
				GetImageGraphFrame(state.Playback),
				state.LastDiagnostic,
				[&] {
					if (!TimelinePinsCurrent(state, state.Dopesheet.OriginalObservationRevision))
						return false;
					const bool accepted =
						ApplyKeyEdit(state, [&](Document &document, uint64_t availableBytes) {
							if (state.Dopesheet.Copying)
								return state.Dopesheet.PrepareCommit(
									document, state.Keys, state.LastDiagnostic, availableBytes
								);
							return WithImageGraphProjectedKeyPins(
								*state.TimelineRead.Snapshot,
								document,
								state.Dopesheet.Originals,
								availableBytes,
								[&](auto pins, uint64_t remaining) {
									return state.Dopesheet.PrepareCommit(
										document, state.Keys, state.LastDiagnostic, remaining, pins
									);
								},
								state.LastDiagnostic,
								state.Dopesheet.OriginalAxes
							);
						});
					if (accepted) state.Dopesheet.PublishCommit(state.Keys);
					return accepted;
				},
				applyKeyTransfer
			);
			if (!state.TimelineRead.Matches(state.GroupHost, state.DocumentRevision)) return;
			state.Keys.Draw(
				*state.TimelineRead.Snapshot,
				GetImageGraphFrame(state.Playback),
				state.LastDiagnostic,
				applyKeyTransfer
			);
			if (!state.TimelineRead.Matches(state.GroupHost, state.DocumentRevision)) return;
			state.EaseKeys.Draw(
				*state.TimelineRead.Snapshot,
				state.TimelineRead.DisplayRevision,
				state.Keys,
				state.LastDiagnostic,
				[&] {
					if (!TimelinePinsCurrent(state, state.EaseKeys.Revision)) return false;
					bool unchanged = false;
					const bool accepted = ApplyKeyEdit(
						state,
						[&](Document &document, uint64_t availableBytes) {
							return WithImageGraphProjectedKeyPins(
								*state.TimelineRead.Snapshot,
								document,
								state.EaseKeys.Originals,
								availableBytes,
								[&](auto pins, uint64_t remaining) {
									return state.EaseKeys.PrepareCommit(
										document, state.LastDiagnostic, remaining, pins
									);
								},
								state.LastDiagnostic,
								state.EaseKeys.OriginalAxes
							);
						},
						&unchanged
					);
					return accepted || unchanged;
				}
			);
			if (!state.TimelineRead.Matches(state.GroupHost, state.DocumentRevision)) return;
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
					const auto rowRevision = state.DocumentRevision;
					const Keyframe &frame = state.Authored.Keyframes[index];
					const std::string rowId = frame.NodeId + "\n" + frame.Port + "\n" +
											  std::to_string(frame.Tick) + "\n" +
											  std::to_string(std::bit_cast<uint64_t>(frame.Subframe)) +
											  (frame.NegativeFrame ? "-" : "+");
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
					ImGui::PushID(rowId.c_str());
					if (ImGui::BeginCombo("##interpolation", interpolation.c_str())) {
						for (const char *choice : {"step", "linear", "cubic", "source"}) {
							const bool selected = interpolation == choice;
							if (ImGui::Selectable(choice, selected)) {
								ApplyPinnedKeyEdit(
									state,
									index,
									[&](Document &document, size_t keyIndex, uint64_t availableBytes) {
										return SetImageGraphKeyframeInterpolation(
											document, keyIndex, choice, state.LastDiagnostic, availableBytes
										);
									}
								);
							}
							if (selected) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
					ImGui::PopID();
					if (rowRevision != state.DocumentRevision) break;
					ImGui::TableSetColumnIndex(3);
					ImGui::PushID(rowId.c_str());
					ImGui::PushID("ease-in");
					DrawKeyframeEaseSide(state, index, state.Authored.Keyframes[index], true);
					ImGui::PopID();
					ImGui::PopID();
					if (rowRevision != state.DocumentRevision) break;
					ImGui::TableSetColumnIndex(4);
					ImGui::PushID(rowId.c_str());
					ImGui::PushID("ease-out");
					DrawKeyframeEaseSide(state, index, state.Authored.Keyframes[index], false);
					ImGui::PopID();
					ImGui::PopID();
					if (rowRevision != state.DocumentRevision) break;
					ImGui::TableSetColumnIndex(5);
					ImGui::PushID(rowId.c_str());
					DrawKeyframeSourceDriver(state, index);
					ImGui::PopID();
					if (rowRevision != state.DocumentRevision) break;
					ImGui::TableSetColumnIndex(6);
					ImGui::PushID(rowId.c_str());
					DrawKeyframeKind(state, index);
					ImGui::PopID();
					if (rowRevision != state.DocumentRevision) break;
					ImGui::TableSetColumnIndex(7);
					ImGui::PushID(rowId.c_str());
					(void)detail::DrawTimelineKeyDelete(
						state.Authored, index, state.LastDiagnostic, [&](const auto &edit) {
							return ApplyKeyEdit(state, edit);
						}
					);
					ImGui::PopID();
				}
				if (state.TimelineRead.Matches(state.GroupHost, state.DocumentRevision))
					(void)detail::DrawTimelineScalarKeys(
						*state.TimelineRead.Snapshot,
						state.Keys,
						state.LastDiagnostic,
						[&](const ImageGraphKeyframeIdentity &key) {
							if (SetImageGraphAuthorFrame(state.Playback, key.Time)) RequestPreview(state);
							if (const auto found = state.Ids.ToCanvas.find(key.NodeId);
								found != state.Ids.ToCanvas.end()) {
								state.Canvas.Select(found->second);
								state.Canvas.Centre(state.Graph, found->second);
							}
						},
						[&](const auto &edit, uint64_t borrowedBytes) {
							return ApplyKeyEdit(state, edit, nullptr, borrowedBytes);
						},
						[&](engine::imagegraph::Curve &curve) { return DrawCurveValue(curve); }
					);

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
				"Audio source selects one exact source ID and tick. Live "
				"devices are not read by the "
				"evaluator."
			);
			ImGui::Separator();
			ImGui::TextUnformatted("Composer source");
			ImGui::InputTextWithHint(
				"##image-pxcx-path", "Path to .pxcx, .pxcc or .pxz", state.PxcxPath, sizeof(state.PxcxPath)
			);
			if (ImGui::Button("Open source")) OpenPxcx(state);
			if (state.ImportedPxcx) {
				ImGui::SameLine();
				if (ImGui::Button("Save PXCX") && SavePxcx(state, false))
					RunAuthoredExports(state, detail::ImageGraphExportEvent::Save);
				ImGui::SameLine();
				if (ImGui::Button("Save PXCX with preview") && SavePxcx(state, true))
					RunAuthoredExports(state, detail::ImageGraphExportEvent::Save);
			}
			if (state.ImportedPxcx) {
				ImGui::InputTextWithHint(
					"##image-pxcx-append-path",
					".pxcx, .pxcc or .pxz to append",
					state.PxcxAppendPath,
					sizeof(state.PxcxAppendPath)
				);
				ImGui::InputText(
					"Append namespace", state.PxcxAppendNamespace, sizeof(state.PxcxAppendNamespace)
				);
				ImGui::InputTextWithHint(
					"Append group",
					"Root, or source group ID",
					state.PxcxAppendContext,
					sizeof(state.PxcxAppendContext)
				);
				ImGui::InputScalarN("Append offset", ImGuiDataType_Double, state.PxcxAppendOffset, 2);
				if (ImGui::Button("Append source")) AppendPxcx(state);
				if (state.CollectionManagers && !state.CollectionManagers->empty()) {
					const auto &collections = *state.CollectionManagers;
					if (std::none_of(collections.begin(), collections.end(), [&](const auto &item) {
							return item.NodeId == state.PxcxCollectionId;
						}))
						state.PxcxCollectionId = collections.front().NodeId;
					if (ImGui::BeginCombo("Collection", state.PxcxCollectionId.c_str())) {
						for (const auto &item : collections)
							if (ImGui::Selectable(item.NodeId.c_str(), item.NodeId == state.PxcxCollectionId))
								state.PxcxCollectionId = item.NodeId;
						ImGui::EndCombo();
					}
					ImGui::InputTextWithHint(
						"##image-pxcc-path",
						"Path to .pxcc or .pxz collection",
						state.PxcxCollectionPath,
						sizeof(state.PxcxCollectionPath)
					);
					ImGui::BeginDisabled(
						state.HaveActiveEdit || state.Playback.Rendering || bool(state.RangeExport)
					);
					ImGui::Checkbox("Include selected preview", &state.PxcxCollectionPreview);
					if (ImGui::Button("Save collection")) SaveCollection(state);
					ImGui::EndDisabled();
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
				if ((state.PublishedPxcx.Archive.OriginalBytes.empty() ? state.PxcxReferenceThumbnail
																	   : state.PublishedPxcx.ReferencePreview)
						.Pixels.empty()) {
					ImGui::TextDisabled("This source has no reference preview.");
				} else {
					ImGui::TextUnformatted("Source preview, reference only.");
					if (UploadPxcxReferenceThumbnail(state, renderer)) {
						void *handle = renderer.TextureHandle(state.CurrentPxcxThumbnailTexture);
						if (handle != nullptr) {
							const auto &preview = state.PublishedPxcx.Archive.OriginalBytes.empty()
													  ? state.PxcxReferenceThumbnail
													  : state.PublishedPxcx.ReferencePreview;
							const float scale = 256.f / float(std::max(preview.Width, preview.Height));
							ImGui::Image(
								reinterpret_cast<ImTextureID>(handle),
								ImVec2(preview.Width * scale, preview.Height * scale)
							);
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
					"Native mappings: %zu  opaque nodes: %zu. Opaque nodes are "
					"preserved and cannot execute.",
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
			ImGui::TextDisabled(
				"Files use Assets/imagegraphs/<name>.graph. PXCX bytes "
				"remain unchanged."
			);
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
				"Preview uses the selected output. Scene texture "
				"bindings are set on the selected entity."
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
				state.PreviewCache.RetainedBytes(),
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
			if (state.PreviewSequence.HaveSequence) {
				auto selection = state.PreviewSequence.Selected();
				ImGui::Text(
					"Sequence %s (%zu members)  displayed frame %.20Lg",
					state.PreviewSequence.Completed.Identity.Output.c_str(),
					selection.Count,
					engine::imagegraph::FrameTimeToReal(state.PreviewSequence.Completed.Frame)
				);
				for (size_t depth = 0; depth < state.PreviewSequence.Levels; ++depth) {
					ImGui::PushID(static_cast<int>(depth));
					uint64_t index = state.PreviewSequence.Indices[depth];
					if (ImGui::InputScalar(depth ? "Nested index" : "Index", ImGuiDataType_U64, &index)) {
						auto indices = state.PreviewSequence.Indices;
						indices[depth] = index;
						const auto candidate = detail::ImageGraphPreviewSequence::Select(
							state.PreviewSequence.Data, {indices.data(), depth + 1}
						);
						if (!candidate.Image || UploadPreview(state, renderer, *candidate.Image)) {
							state.PreviewSequence.Indices = indices;
							state.PreviewSequence.Levels = depth + 1;
							if (!candidate.Image) state.HaveGoodPreview = false;
							selection = candidate;
						}
					}
					ImGui::PopID();
				}
				if (selection.Branch && state.PreviewSequence.Levels < state.PreviewSequence.Indices.size() &&
					ImGui::Button("Open nested sequence")) {
					const size_t levels = state.PreviewSequence.Levels + 1;
					auto indices = state.PreviewSequence.Indices;
					indices[levels - 1] = 0;
					const auto candidate = detail::ImageGraphPreviewSequence::Select(
						state.PreviewSequence.Data, {indices.data(), levels}
					);
					if (!candidate.Image || UploadPreview(state, renderer, *candidate.Image)) {
						state.PreviewSequence.Indices = indices;
						state.PreviewSequence.Levels = levels;
						selection = candidate;
						if (!candidate.Image) state.HaveGoodPreview = false;
					}
				}
				if (!selection.Image)
					ImGui::TextUnformatted(
						selection.Branch ? "Selected member is a nested sequence."
										 : "Selected sequence is empty."
					);
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

	} // namespace

	void CloseImageComposerVector2Preview(engine::render::Renderer &renderer) {
		Composer().VectorControls.Close(renderer);
		Composer().NodePreviews.Close(renderer);
	}

	void CloseImageComposerAudioPreview() {
		State &state = Composer();
		state.Playback.Playing = state.Playback.Rendering = false;
		state.Playback.FrameProgress = false;
		state.Playback.LastTime = 0;
		state.WavAudio.Close();
		state.WavAudioMessage.clear();
	}

	bool PrepareImageComposerArguments(
		const engine::imagegraph::SourceArgumentOptions &options,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		State &state = Composer();
		if (state.Initialized) {
			diagnostic = {
				Status::InvalidValue,
				{},
				"arguments",
				"Running Composer argument replacement requires its renderer"
			};
			return false;
		}
		return detail::PrepareImageGraphArguments(
			state.Host.SourceArguments,
			options,
			state.EvaluationInputRevision,
			[] {},
			diagnostic,
			maximumBytes
		);
	}
	bool PrepareImageComposerArguments(
		const engine::imagegraph::SourceArgumentOptions &options,
		engine::render::Renderer &renderer,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		State &state = Composer();
		return detail::PrepareImageGraphArguments(
			state.Host.SourceArguments,
			options,
			state.EvaluationInputRevision,
			[&] {
				CancelExportIntent(state, renderer);
				CancelComposerPreview(state, renderer);
				ClearFeedbackHost(state);
				state.Host.ResetFiles();
				state.ComposerExports.Pending.clear();
				state.ExportUpdate = {};
				state.PreviewCache.Clear();
				state.PreviewSequence.Invalidate();
				state.PxcxCompletedPreview.reset();
				state.PcxObservations.Captured = false;
				RequestPreview(state, true);
			},
			diagnostic,
			maximumBytes
		);
	}

	void ResetImageComposerPanelLayout() {
		auto &panels = Composer().Panels;
		panels.LayoutInitialized = false;
		panels.ResetRequested = true;
	}

	bool ImageComposerHasPendingCapture() {
		return Composer().ComposerDevicePending;
	}

	void DrawImageComposer(engine::render::Renderer &renderer, engine::core::Name owner, bool &open) {
		State &state = Composer();
		Initialize(state);
		const auto paths = engine::assets::DefaultLocalPaths();
		if (!state.ComposerCaptureNames.empty() &&
			(state.ComposerCadence.Current.Owner != owner || !open ||
			 state.ComposerCadence.Current.Revision != state.DocumentRevision ||
			 state.ComposerCadence.Current.InputRevision != state.EvaluationInputRevision))
			CancelComposerPreview(state, renderer);
		detail::ImageGraphComposerHost composerHost(renderer, owner, paths, &state.ComposerCaptureNames);
		const detail::ImageGraphComposerHostScope composerScope(state.Host.Composer, composerHost);
		if (state.ExportIntent.Current &&
			(!open ||
			 !state.ExportIntent.Matches(owner, state.DocumentRevision, state.EvaluationInputRevision)))
			CancelExportIntent(state, renderer);
		struct RetainPendingSignal {
			State &Owner;
			const detail::ImageGraphComposerHost &Host;
			~RetainPendingSignal() {
				Owner.ComposerDevicePending = Host.HavePendingJobs;
			}
		} pendingSignal{state, composerHost};

		if (state.WavCheckerHostFrame != std::numeric_limits<uint64_t>::max()) {
			engine::imagegraphphysics::RigidProvider checkerRigidProvider;
			engine::imagegraph::EvaluationRequest checkerRequest;
			checkerRequest.HostProvider = &HostFor(state);
			engine::imagegraph::SourceFontContext checkerRequestFontContext;
			if (!BindObservations(state, checkerRequest, &checkerRequestFontContext)) {
				state.WavSourceMessage = state.LastDiagnostic.Message;
			} else {
				detail::BindImageGraphRigid(checkerRequest, checkerRigidProvider, state.Playback);
				(void)SetFrameTime(checkerRequest, GetImageGraphFrame(state.Playback));
				checkerRequest.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
				size_t reloaded = 0;
				Diagnostic diagnostic;
				const bool checked = state.WavAudio.CheckFiles(
					state.Authored,
					checkerRequest,
					state.WavCheckerHostFrame++,
					state.AudioClips,
					state.PreviewCache,
					reloaded,
					diagnostic
				);
				if (reloaded) {
					CancelComposerPreview(state, renderer);
					state.WavSourceMessage.clear();
					if (++state.EvaluationInputRevision == 0) state.EvaluationInputRevision = 1;
					RequestPreview(state, true);
				}
				if (!checked) state.WavSourceMessage = diagnostic.Message;
			}
		}
		PumpRetiredTexture(state, renderer);
		state.VectorControls.PumpRetired(renderer);

		bool playbackAdvanced = false;
		if (CommitSourcePlaybackTransition(
				state,
				[&](ImageGraphPlayback &playback) {
					playbackAdvanced = detail::AdvanceAnimationPlayback(playback, ImGui::GetIO().DeltaTime);
					return true;
				}
			) &&
			playbackAdvanced)
			RequestPreview(state);
		RunAnimationControls(state, renderer);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
		if (open) ResumeExportIntent(state);
		const bool ownerVisible = ImGui::Begin("Image Composer", &open);
		if (!open) {
			ImGui::End();
			ImGui::PopStyleColor(2);
			state.PreviewSequence.Clear();
			CancelExportIntent(state, renderer);
			CancelComposerPreview(state, renderer);
			if (state.LuaHost) state.LuaHost->Reset();
			state.Host.ResetFiles();
			state.ExportUpdate = {};
			CloseImageComposerAudioPreview();
			state.VectorControls.Close(renderer);
			state.NodePreviews.Close(renderer);
			return;
		}
		FinishInactiveEdit(state);
		if (ownerVisible) DrawToolbar(state);
		engine::imagegraphphysics::RigidProvider vectorRequestRigidProvider;
		engine::imagegraph::EvaluationRequest vectorRequest;
		vectorRequest.HostProvider = &HostFor(state);
		engine::imagegraph::SourceFontContext vectorRequestFontContext;
		bool vectorInputsBound = BindObservations(state, vectorRequest, &vectorRequestFontContext);
		if (vectorInputsBound) {
			detail::BindImageGraphRigid(vectorRequest, vectorRequestRigidProvider, state.Playback);
			(void)engine::imagegraph::SetFrameTime(vectorRequest, GetImageGraphFrame(state.Playback));
			vectorRequest.AudioFrames = state.AudioFrames;
			vectorRequest.AudioClips = state.AudioClips;
			vectorRequest.MaximumImageDimension = PREVIEW_MAXIMUM_DIMENSION;
			if (!AdvanceSourceCommonRuntime(state, vectorRequest, state.LastDiagnostic))
				vectorInputsBound = false;
			vectorRequest.GroupRender = state.FeedbackHost.PreparedGroups(
				state.DocumentRevision, state.EvaluationInputRevision, GetImageGraphFrame(state.Playback)
			);
			state.VectorControls.Refresh(
				state.Authored, vectorRequest, state.DocumentRevision, state.EvaluationInputRevision
			);
		}
		if (vectorInputsBound && (state.LivePreview || state.PreviewRequested) &&
			(state.Authored.FormatVersion < 10 || state.Authored.Groups.empty() || vectorRequest.GroupRender))
			state.NodePreviews.Refresh(
				state.Authored,
				vectorRequest,
				HostFor(state),
				renderer,
				state.DocumentRevision,
				state.EvaluationInputRevision,
				detail::ImageGraphPlaybackObservation(state.Authored, state.Playback)
			);
		const ImGuiWindowClass composerClass = detail::ImageComposerWindowClass();
		if (ownerVisible) {
			ImGui::Separator();
			detail::InitializeImageComposerPanels(
				state.Panels, ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail()
			);
			ImGui::DockSpace(
				detail::ImageComposerDockspaceId(), ImVec2(0, 0), ImGuiDockNodeFlags_None, &composerClass
			);
			detail::ApplyImageGraphHistoryKey([&](bool redo) { ApplyHistory(state, redo); });
		} else {
			if (!state.Panels.LayoutInitialized && !state.Panels.ResetRequested &&
				ImGui::DockBuilderGetNode(detail::ImageComposerDockspaceId()) != nullptr &&
				ImGui::FindWindowSettingsByID(ImHashStr(detail::IMAGE_COMPOSER_GRAPH)) != nullptr)
				detail::InitializeImageComposerPanels(
					state.Panels, ImGui::GetWindowPos(), ImGui::GetWindowSize()
				);
			ImGui::DockSpace(
				detail::ImageComposerDockspaceId(),
				ImVec2(0, 0),
				ImGuiDockNodeFlags_KeepAliveOnly,
				&composerClass
			);
		}
		ImGui::End();
		if (state.Panels.LayoutInitialized) {
			ImGui::SetNextWindowClass(&composerClass);
			if (ImGui::Begin(detail::IMAGE_COMPOSER_GRAPH)) {
				if (ImGui::BeginChild("##image-graph-canvas", ImVec2(0, 0), false)) {
					if (state.CanvasNeedsReload) ReloadCanvas(state);
					RefreshCacheGroupMarks(state);
					state.Canvas.Draw(state.Graph);
					ApplyPendingCacheGroupClick(state);
					if (state.CanvasNeedsReload) ReloadCanvas(state);
				}
				ImGui::EndChild();
				detail::ApplyImageGraphHistoryKey([&](bool redo) { ApplyHistory(state, redo); });
			}
			ImGui::End();
			for (size_t index = 0; index < detail::IMAGE_COMPOSER_PANELS.size(); ++index) {
				if (!state.Panels.Open[index]) continue;
				ImGui::SetNextWindowClass(&composerClass);
				const bool visible =
					ImGui::Begin(detail::IMAGE_COMPOSER_PANELS[index].Window, &state.Panels.Open[index]);
				if (visible) {
					switch (index) {
					case 0:
						DrawPalette(state);
						break;
					case 1:
						DrawInspector(state, renderer, owner, paths);
						break;
					case 2:
						DrawProjectSettings(state);
						break;
					case 3:
						DrawGroups(state);
						break;
					case 4:
						DrawOutputs(state);
						ImGui::Separator();
						RefreshPreview(state, renderer);
						DrawPreview(state, renderer);
						break;
					case 5:
						DrawAssetsAndSinks(state, renderer);
						break;
					case 6:
						DrawFontInputs(state);
						break;
					case 7:
						if (PrepareTimelineRead(state))
							DrawTimeline(state);
						else
							ImGui::TextWrapped("%s", state.LastDiagnostic.Message.c_str());
						break;
					case 8:
						RefreshPreview(state, renderer);
						DrawDiagnostics(state);
						if (!state.WavAudioMessage.empty())
							ImGui::TextWrapped("%s", state.WavAudioMessage.c_str());
						break;
					}
					detail::ApplyImageGraphHistoryKey([&](bool redo) { ApplyHistory(state, redo); });
				}
				ImGui::End();
			}
		}
		ApplyPendingFontInputs(state, renderer);
		RefreshPreview(state, renderer);
		state.ComposerExports.Invalidate(state.DocumentRevision, state.EvaluationInputRevision);
		if (!state.Playback.Rendering &&
			std::any_of(state.Authored.Nodes.begin(), state.Authored.Nodes.end(), [](const auto &node) {
				return node.Type == "pc.export";
			})) {
			auto update = state.ExportUpdate;
			if (update.Accept(
					state.DocumentRevision, state.EvaluationInputRevision, GetImageGraphFrame(state.Playback)
				)) {
				engine::imagegraph::EvaluationRequest captured;
				BindObservations(state, captured);
				std::string failure;
				if (state.ComposerExports.Admit(
						{state.Playback,
						 state.PcxObservations,
						 state.DocumentRevision,
						 state.EvaluationInputRevision},
						failure
					))
					state.ExportUpdate = update;
				else
					state.LastDiagnostic = {
						Status::LimitExceeded, {}, "export_on_update", std::move(failure)
					};
			}
		}
		if (!state.ExportIntent.Current && !state.ComposerCadence.Held &&
			!state.ComposerExports.Pending.empty()) {
			RunAuthoredExports(
				state, detail::ImageGraphExportEvent::Update, {}, &state.ComposerExports.Pending.front()
			);
		}

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
		state.VectorControls.FinishPointer(
			ImGui::IsMouseDown(ImGuiMouseButton_Left), ImGui::IsMouseDown(ImGuiMouseButton_Middle)
		);
		ImGui::PopStyleColor(2);
	}
} // namespace studio
