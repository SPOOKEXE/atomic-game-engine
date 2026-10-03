#include "AuthoredExportInternal.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/BuiltinRandomFile.hpp>
#include <engine/imagegraphexport/GraphExportSession.hpp>
#include <engine/imagegraphexport/GraphInputs.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>
namespace engine::imagegraphexport {
	struct GraphExportSession::State {
		using Image = engine::imagegraph::Image;
		struct Frame {
			size_t Target = 0;
			uint64_t Tick = 0, Hash = 0, Bytes = 0;
			uint32_t Width = 0, Height = 0;
			engine::imagegraph::SurfaceFormat Format{};
		};
		engine::imagegraph::Document Document;
		engine::imagegraph::Plan Plan;
		std::vector<GraphExportSettings> Exports;
		std::vector<Frame> Frames;
		std::filesystem::path Directory;
		engine::imagegraph::CapturedFeedbackHost Replay;
		engine::imagegraph::PendingHostObservations Observations;
		GraphExportGeneration Generation;
		size_t Target = 0;
		uint64_t Tick = 0, SelectedTick = 0, StagedBytes = 0, Seed = 0;
		uint32_t MaximumDimension = 0;
		bool RigidPlaying = false, RigidProgress = false;
		bool Complete = false, Temporal = false;
		std::vector<engine::imagegraph::RequestImageSource> ImageSources;
		std::vector<engine::imagegraph::SourceBuiltinRandomCapture> RandomCaptures;
		std::vector<std::filesystem::path> Retained;
		static constexpr uint64_t MaximumStagedBytes = 512ull * 1024 * 1024;
		~State() {
			std::error_code error;
			if (!Directory.empty()) std::filesystem::remove_all(Directory, error);
		}
		std::filesystem::path FramePath(size_t target, uint64_t tick) const {
			return Directory / (std::to_string(target) + "-" + std::to_string(tick) + ".cframe");
		}
		struct Observer : engine::imagegraph::HostNodeProvider {
			State &Session;
			GraphExportSessionHost &Host;
			const engine::imagegraph::EvaluationRequest &Request;
			Observer(
				State &session,
				GraphExportSessionHost &host,
				const engine::imagegraph::EvaluationRequest &request
			)
				: Session(session), Host(host), Request(request) {}
			bool Capture(
				const engine::imagegraph::HostNodeInvocation &call,
				engine::imagegraph::HostNodeCapture &out,
				std::string &failure
			) override {
				auto bounded = call;
				bounded.MaximumOperationBytes = std::min(
					call.MaximumOperationBytes, engine::imagegraph::Limits::MaximumEvaluationBytes / 4
				);
				return Session.Observations.CaptureSequenced(bounded, Host, out, failure);
			}
			bool PcxMessages(
				std::string_view node,
				std::span<const engine::imagegraph::PcxMessage> messages,
				std::string &failure
			) override {
				return Session.Observations.ForwardMessages(
					Request,
					node,
					messages,
					Host,
					engine::imagegraph::Limits::MaximumEvaluationBytes / 4,
					failure
				);
			}
		};
		bool Store(const Image &image, std::string &failure) {
			ENGINE_PROFILE("image composer export frame stage");
			using namespace engine::imagegraph;
			const auto remaining = MaximumStagedBytes - std::min(StagedBytes, MaximumStagedBytes);
			if (!ValidSurfaceLayout(image, MaximumDimension, Limits::MaximumEvaluationBytes / 2) ||
				!FiniteSurfaceSamples(image) || image.Pixels.size() + 256 > remaining) {
				failure = "export frame layout or aggregate staged bytes exceed bounds";
				return false;
			}
			Frame record{
				Target, Tick, SurfaceHash(image), image.Pixels.size(), image.Width, image.Height, image.Format
			};
			const auto format = DescribeSurfaceFormat(image.Format);
			if (!format) return false;
			std::ostringstream header;
			header << "AGE_GRAPH_EXPORT_FRAME 1 " << format->Name << ' ' << record.Width << ' '
				   << record.Height << ' ' << record.Hash << ' ' << record.Bytes << '\n';
			const auto text = header.str();
			std::ofstream file(FramePath(Target, Tick), std::ios::binary | std::ios::trunc);
			file.write(text.data(), static_cast<std::streamsize>(text.size()));
			file.write(
				reinterpret_cast<const char *>(image.Pixels.data()),
				static_cast<std::streamsize>(image.Pixels.size())
			);
			file.close();
			if (!file) {
				failure = "cannot stage complete typed export frame";
				return false;
			}
			Frames.push_back(record); // All frame slots were reserved and admitted at Begin.
			StagedBytes += text.size() + image.Pixels.size();
			engine::core::Metrics::Count(
				"image composer staged frame bytes", static_cast<double>(text.size() + image.Pixels.size())
			);
			engine::core::Metrics::Count("image composer staged frames", 1);
			return true;
		}
		struct CompletedFrames : engine::imagegraph::HostNodeProvider {
			State &Session;
			size_t Target = 0;
			explicit CompletedFrames(State &session) : Session(session) {}
			bool Capture(
				const engine::imagegraph::HostNodeInvocation &call,
				engine::imagegraph::HostNodeCapture &out,
				std::string &failure
			) override {
				ENGINE_PROFILE("image composer staged frame read");
				using namespace engine::imagegraph;
				const auto found =
					std::find_if(Session.Frames.begin(), Session.Frames.end(), [&](const auto &frame) {
						return frame.Target == Target && frame.Tick == call.Request.Tick;
					});
				if (found == Session.Frames.end() || call.Authored.Id != "__completed_export_frame") {
					failure = "completed export frame is absent";
					return false;
				}
				uint64_t bytes = 0;
				Diagnostic diagnostic;
				HostNodeCapture candidate;
				if (PrepareResolvedHostCapture(
						call, call.MaximumOperationBytes, candidate, bytes, diagnostic
					) != Status::Ok ||
					found->Bytes + 4096 >
						call.MaximumOperationBytes - std::min(bytes, call.MaximumOperationBytes)) {
					failure = "completed export frame receipt exceeds byte bounds";
					return false;
				}
				const auto descriptor = DescribeSurfaceFormat(found->Format);
				if (!descriptor) {
					failure = "completed frame format is invalid";
					return false;
				}
				std::ostringstream header;
				header << "AGE_GRAPH_EXPORT_FRAME 1 " << descriptor->Name << ' ' << found->Width << ' '
					   << found->Height << ' ' << found->Hash << ' ' << found->Bytes << '\n';
				const auto text = header.str();
				const auto path = Session.FramePath(Target, found->Tick);
				std::error_code error;
				const auto status = std::filesystem::symlink_status(path, error);
				if (error || status.type() != std::filesystem::file_type::regular ||
					std::filesystem::file_size(path, error) != found->Bytes + text.size() || error ||
					text.size() > 256) {
					failure = "completed export frame file type or size changed";
					return false;
				}
				std::ifstream file(path, std::ios::binary);
				std::array<char, 256> actual{};
				file.read(actual.data(), static_cast<std::streamsize>(text.size()));
				if (!file || std::string_view(actual.data(), text.size()) != text) {
					failure = "completed export frame header changed";
					return false;
				}
				const auto count = found->Bytes;
				const auto width = found->Width, height = found->Height;
				const auto hash = found->Hash;
				Image image;
				image.Width = width;
				image.Height = height;
				image.Format = found->Format;
				image.Pixels.resize(static_cast<size_t>(count));
				if (image.Pixels.capacity() + bytes + 4096 > call.MaximumOperationBytes) {
					failure = "completed export frame capacity exceeds byte bounds";
					return false;
				}
				file.read(reinterpret_cast<char *>(image.Pixels.data()), static_cast<std::streamsize>(count));
				if (!file || file.peek() != std::char_traits<char>::eof() || SurfaceHash(image) != hash ||
					!FiniteSurfaceSamples(image)) {
					failure = "completed export frame pixels changed";
					return false;
				}
				image.Hash = hash;
				candidate.Images.push_back({"surface_out", std::move(image)});
				candidate.Outputs = {
					{"path", std::string{}}, {"dimension", Vector2{double(width), double(height)}}
				};
				out = std::move(candidate);
				engine::core::Metrics::Count(
					"image composer staged frame read bytes", static_cast<double>(count)
				);
				return true;
			}
		};
	};

	GraphExportSession::GraphExportSession() = default;
	GraphExportSession::~GraphExportSession() = default;
	std::optional<engine::imagegraph::FrameTime> GraphExportSession::NextFrame() const {
		if (!Inside || Inside->Complete || Inside->Target >= Inside->Exports.size()) return std::nullopt;
		return engine::imagegraph::FrameTime{Inside->Tick, 0, false};
	}
	std::span<const std::filesystem::path> GraphExportSession::RetainedDirectories() const {
		return Inside ? std::span<const std::filesystem::path>(Inside->Retained)
					  : std::span<const std::filesystem::path>{};
	}
	uint64_t GraphExportSession::MaximumPayloadReservationBytes() noexcept {
		return 2 * engine::imagegraph::Limits::MaximumEvaluationBytes + sizeof(State);
	}
	uint64_t GraphExportSession::PayloadReservationBytes() const noexcept {
		return Inside ? MaximumPayloadReservationBytes() : 0;
	}
	void GraphExportSession::Cancel(GraphExportSessionHost &host) noexcept {
		host.Cancel();
		Inside.reset();
	}
	bool GraphExportSession::BeginAuthored(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		const engine::imagegraph::EvaluationRequest &request,
		const GraphExportSettings &grants,
		std::string_view nodeId,
		GraphExportGeneration generation,
		std::string &failure
	) try {
		ENGINE_PROFILE("image composer export session admission");
		using namespace engine::imagegraph;
		if (Inside) {
			failure = "cancel the prior export session before admitting another intent";
			return false;
		}
		const auto bytes = DocumentRetainedPayloadBytes(document);
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
			return item.Id == nodeId && item.Type == "pc.export";
		});
		if (!bytes || *bytes > Limits::MaximumEvaluationBytes / 8 || node == document.Nodes.end()) {
			failure = "export session document is absent or exceeds retained copy bounds";
			return false;
		}
		auto candidate = std::make_unique<State>();
		if (snapshot.RetainedBytes() > Limits::MaximumEvaluationBytes / 8) {
			failure = "export session prepared controls exceed their admission budget";
			return false;
		}
		candidate->Document = document;
		auto resolvedGrants = grants;
		auto output = std::find_if(
			candidate->Document.Outputs.begin(), candidate->Document.Outputs.end(), [&](const auto &item) {
				return item.NodeId == nodeId && item.Port == "preview" &&
					   (resolvedGrants.OutputId.empty() || item.Id == resolvedGrants.OutputId);
			}
		);
		if (output == candidate->Document.Outputs.end()) {
			if (!resolvedGrants.OutputId.empty() ||
				candidate->Document.Outputs.size() >= Limits::MaximumOutputs) {
				failure = "export session selected output does not bind the authored preview";
				return false;
			}
			std::string name = "__export_session_preview";
			for (size_t index = 0; std::any_of(
					 candidate->Document.Outputs.begin(),
					 candidate->Document.Outputs.end(),
					 [&](const auto &item) { return item.Id == name; }
				 );
				 ++index)
				name = "__export_session_preview_" + std::to_string(index);
			candidate->Document.Outputs.push_back({name, std::string(nodeId), "preview"});
			resolvedGrants.OutputId = name;
		} else
			resolvedGrants.OutputId = output->Id;
		size_t imageCount = 1;
		double type = 0;
		if (!detail::PlanPreparedAuthoredExport(
				document,
				*node,
				snapshot,
				request,
				resolvedGrants,
				{},
				candidate->Exports,
				imageCount,
				type,
				failure
			))
			return false;
		if (type == 0) {
			failure = "single exports use the prepared surface export API";
			return false;
		}
		size_t count = 0;
		Diagnostic diagnostic;
		for (auto &settings : candidate->Exports) {
			size_t frames = 0;
			if (ValidateTickRange(settings.Frames, frames, diagnostic) != Status::Ok ||
				frames > Limits::MaximumRangeFrames - std::min(count, Limits::MaximumRangeFrames)) {
				failure = "export session exceeds its aggregate 4096 frame bound";
				return false;
			}
			count += frames;
			settings.HostProvider = nullptr;
			settings.HostCaptures = {};
			settings.ImageInputs.clear();
			settings.BuiltinRandomCapture.clear();
		}
		if (!count) {
			failure = "export session has no requested frames";
			return false;
		}
		candidate->Frames.reserve(count);
		if (candidate->Frames.capacity() * sizeof(State::Frame) > Limits::MaximumArrayBytes) {
			failure = "export frame metadata exceeds byte bounds";
			return false;
		}
		const auto copied = DocumentRetainedPayloadBytes(candidate->Document);
		if (!copied || *copied > Limits::MaximumEvaluationBytes / 8) {
			failure = "export session document capacity exceeds byte bounds";
			return false;
		}
		if (Compile(candidate->Document, candidate->Plan, diagnostic) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		size_t work = 0;
		for (const auto &settings : candidate->Exports) {
			const std::array<std::string, 1> ids{settings.OutputId};
			const auto cone = AnalyzeStatefulTemporalCone(candidate->Document, candidate->Plan, ids);
			if (!cone.Valid) {
				failure = "export session temporal cone is invalid";
				return false;
			}
			candidate->Temporal |= cone.Simulation || cone.SurfaceCaches || cone.RandomGenerators ||
								   cone.RigidActors || cone.FirstFrameData;
		}
		for (const auto &item : candidate->Document.Nodes)
			for (const auto &value : item.Values)
				if (item.Type == "image.captured" && value.Port == "source_id")
					if (const auto *name = std::get_if<std::string>(&value.Data);
						name && name->starts_with("feedback:"))
						candidate->Temporal = true;
		for (const auto &settings : candidate->Exports) {
			if (candidate->Temporal && settings.Frames.Last >= Limits::MaximumRangeFrames) {
				failure = "export session temporal warmup exceeds its 4096 step work bound";
				return false;
			}
			const uint64_t ticks =
				candidate->Temporal
					? settings.Frames.Last + 1
					: (settings.Frames.Last - settings.Frames.First) / settings.Frames.Step + 1;
			if (ticks > Limits::MaximumRangeFrames - std::min(work, Limits::MaximumRangeFrames)) {
				failure = "export session including temporal warmup exceeds its 4096 step work bound";
				return false;
			}
			work += static_cast<size_t>(ticks);
		}
		if (!LoadGraphImageInputs(
				grants, candidate->ImageSources, failure, Limits::MaximumEvaluationBytes / 8
			))
			return false;
		if (!grants.BuiltinRandomCapture.empty() && !LoadBuiltinRandomCaptureFile(
														grants.BuiltinRandomCapture,
														grants.Content,
														candidate->RandomCaptures,
														failure,
														Limits::MaximumEvaluationBytes / 8
													))
			return false;
		std::error_code error;
		const auto absolute = std::filesystem::absolute(grants.Output, error);
		const auto root =
			error ? std::filesystem::path{} : std::filesystem::weakly_canonical(absolute, error);
		if (error || root.empty() || !grants.Content.AllowsName(root.string()) ||
			!std::filesystem::is_directory(root, error) || error) {
			failure = "export session requires an existing explicitly granted output directory";
			return false;
		}
		static std::atomic<uint64_t> serial{0};
		bool created = false;
		for (size_t attempt = 0; attempt < 64 && !created; ++attempt) {
			auto path = root / (".graph-export-session-" + std::to_string(serial.fetch_add(1)));
			if (!grants.Content.AllowsName(path.string())) {
				failure = "export session staging is refused by content policy";
				return false;
			}
			created = std::filesystem::create_directory(path, error);
			if (error && error != std::errc::file_exists) break;
			if (created) candidate->Directory = std::move(path);
		}
		if (!created) {
			failure = "cannot create exclusive export session staging";
			return false;
		}
		candidate->Generation = generation;
		candidate->SelectedTick = candidate->Exports.front().Frames.First;
		candidate->Tick = candidate->Temporal ? 0 : candidate->SelectedTick;
		candidate->Seed = request.Seed;
		candidate->MaximumDimension = request.MaximumImageDimension;
		candidate->RigidPlaying = request.RigidPlaying;
		candidate->RigidProgress = request.RigidFrameProgress;
		Inside = std::move(candidate);
		return true;
	} catch (const std::bad_alloc &) {
		failure = "export session admission allocation exceeded bounds";
		return false;
	}

	GraphExportProgress GraphExportSession::Resume(
		const engine::imagegraph::EvaluationRequest &observations,
		GraphExportGeneration generation,
		GraphExportSessionHost &host,
		std::string &failure
	) try {
		ENGINE_PROFILE("image composer export session resume");
		using namespace engine::imagegraph;
		const auto fail = [&](std::string message) {
			failure = std::move(message);
			Cancel(host);
			return GraphExportProgress::Failed;
		};
		if (!Inside) return fail("export session is not admitted");
		auto &state = *Inside;
		if (state.Generation != generation || state.Seed != observations.Seed ||
			state.MaximumDimension != observations.MaximumImageDimension ||
			state.RigidPlaying != observations.RigidPlaying ||
			state.RigidProgress != observations.RigidFrameProgress)
			return fail("export session immutable generations or observations changed");
		if (state.Complete) return GraphExportProgress::Complete;
		if (state.Target < state.Exports.size()) {
			auto request = observations;
			state.Observations.BeginAttempt();
			State::Observer observer(state, host, request);
			request.Tick = state.Tick;
			request.Subframe = 0;
			request.NegativeFrame = false;
			request.HostProvider = &observer;
			if (!state.ImageSources.empty()) request.ImageSources = state.ImageSources;
			if (!state.RandomCaptures.empty()) request.BuiltinRandomCaptures = state.RandomCaptures;
			request.SimulationReplay = nullptr;
			request.SurfaceReplay = nullptr;
			request.RandomReplay = nullptr;
			request.DataReplay = nullptr;
			request.RigidReplay = nullptr;
			request.ReuseSimulationFrame = false;
			engine::imagegraphphysics::RigidProvider rigid;
			if (!request.RigidProvider) request.RigidProvider = &rigid;
			Diagnostic diagnostic;
			const auto &settings = state.Exports[state.Target];
			if (!state.Replay.Prepare(
					state.Document,
					state.Plan,
					generation.Authoring,
					generation.Inputs,
					request,
					diagnostic,
					Limits::MaximumEvaluationBytes / 2,
					settings.OutputId
				)) {
				if (host.Pending()) {
					failure.clear();
					return GraphExportProgress::Pending;
				}
				return fail(diagnostic.Message);
			}
			Image direct;
			ImageArray array;
			const Image *selected = nullptr;
			const auto *replayed = state.Replay.Active() ? state.Replay.Value(settings.OutputId) : nullptr;
			if (settings.ArrayIndex) {
				const ImageArray *members = nullptr;
				if (replayed)
					members = std::get_if<ImageArray>(&replayed->Output);
				else {
					if (EvaluateArray(
							state.Document, state.Plan, settings.OutputId, request, array, diagnostic
						) != Status::Ok) {
						if (host.Pending()) {
							failure.clear();
							return GraphExportProgress::Pending;
						}
						return fail(diagnostic.Message);
					}
					members = &array;
				}
				if (!members || *settings.ArrayIndex >= members->Items.size())
					return fail("export array member is absent");
				const auto *index = std::get_if<size_t>(&members->Items[*settings.ArrayIndex].Data);
				if (!index || *index >= members->Images.size())
					return fail("export array member is nested or invalid");
				selected = &members->Images[*index];
			} else if (replayed)
				selected = state.Replay.Output(settings.OutputId);
			else {
				if (Evaluate(state.Document, state.Plan, settings.OutputId, request, direct, diagnostic) !=
					Status::Ok) {
					if (host.Pending()) {
						failure.clear();
						return GraphExportProgress::Pending;
					}
					return fail(diagnostic.Message);
				}
				selected = &direct;
			}
			if (!selected) return fail("export replay output is not an image");
			if (state.Tick == state.SelectedTick && !state.Store(*selected, failure)) return fail(failure);
			state.Observations.Clear();
			if (state.Tick < state.SelectedTick)
				++state.Tick;
			else if (settings.Frames.Last - state.SelectedTick >= settings.Frames.Step) {
				state.SelectedTick += settings.Frames.Step;
				state.Tick = state.Temporal ? state.Tick + 1 : state.SelectedTick;
			} else {
				++state.Target;
				if (state.Target < state.Exports.size()) {
					const auto &next = state.Exports[state.Target];
					const bool restart = next.OutputId != settings.OutputId ||
										 next.ArrayIndex != settings.ArrayIndex ||
										 next.Frames.First <= state.Tick;
					if (restart) state.Replay.Clear();
					state.SelectedTick = next.Frames.First;
					state.Tick = state.Temporal ? (restart ? 0 : state.Tick + 1) : state.SelectedTick;
				}
			}
			return GraphExportProgress::Progress;
		}
		// Encoding reads owned completed frames only. Original host effects and replay owners cannot run
		// again.
		state.Replay.Clear();
		state.Observations.Clear();
		state.Document = {};
		state.Plan = {};
		state.ImageSources = {};
		state.RandomCaptures = {};
		State::CompletedFrames provider(state);
		Document captured;
		captured.FormatVersion = 9;
		captured.Nodes.push_back({"__completed_export_frame", "pc.image", "", {}, {{"path", std::string{}}}});
		captured.Outputs.push_back({"__completed_export_frame", "__completed_export_frame", "surface_out"});
		Plan plan;
		Diagnostic diagnostic;
		if (Compile(captured, plan, diagnostic) != Status::Ok) return fail(diagnostic.Message);
		auto settings = state.Exports;
		for (auto &item : settings) {
			item.OutputId = "__completed_export_frame";
			item.ArrayIndex.reset();
			item.HostProvider = &provider;
		}
		EvaluationRequest request;
		request.MaximumImageDimension = state.MaximumDimension;
		request.HostProvider = &provider;
		const auto select = [](void *context, size_t target) {
			static_cast<State::CompletedFrames *>(context)->Target = target;
		};
		if (!detail::ExportBatch(
				settings, captured, plan, request, true, failure, &state.Retained, select, &provider
			))
			return fail(failure);
		host.Cancel();
		state.Complete = true;
		state.Replay.Clear();
		state.Observations.Clear();
		std::error_code error;
		std::filesystem::remove_all(state.Directory, error);
		state.Directory.clear();
		failure.clear();
		return GraphExportProgress::Complete;
	} catch (const std::bad_alloc &) {
		failure = "export session resume allocation exceeded bounds";
		Cancel(host);
		return GraphExportProgress::Failed;
	}

}
