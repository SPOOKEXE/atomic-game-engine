#include "FrameObservation.hpp"

#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace client {
	struct FrameObservationState {
		const Client *Identity = nullptr;
		std::thread::id Owner;
		FrameObservationOptions Configuration;
		FrameObservationCallback Callback;
		uint64_t Generation = 0;
		bool Started = false;
		bool Finished = false;
		std::optional<uint64_t> LastClientFrame;
		uint64_t LastRendererFrame = 0;
		std::atomic<bool> Closed = false;
		std::atomic<size_t> Bytes = 0;
		std::atomic<size_t> Frames = 0;
		std::atomic<uint64_t> Delivered = 0;
		std::mutex FailureMutex;
		std::string Error;
		uint64_t PendingToken = 0;
		engine::render::Renderer *PendingRenderer = nullptr;
		size_t PendingBytes = 0;
		std::unique_ptr<FrameObservation> Pending;
	};

	namespace {
		struct ClientLifetime {
			std::thread::id Owner;
			bool Started = false;
			std::shared_ptr<FrameObservationState> Subscription;
		};
		struct Registry {
			std::mutex Mutex;
			std::unordered_map<const Client *, ClientLifetime> Clients;
			uint64_t NextGeneration = 1;
		};
		Registry &Subscriptions() {
			static Registry registry;
			return registry;
		}
		void Fail(const std::shared_ptr<FrameObservationState> &state, std::string error) {
			std::lock_guard lock(state->FailureMutex);
			if (state->Error.empty()) state->Error = std::move(error);
		}
		std::shared_ptr<FrameObservationState> Subscription(const Client &client) {
			auto &registry = Subscriptions();
			std::lock_guard lock(registry.Mutex);
			const auto found = registry.Clients.find(&client);
			return found == registry.Clients.end() ? nullptr : found->second.Subscription;
		}
		bool OnOwner(const std::shared_ptr<FrameObservationState> &state) {
			if (state->Owner == std::this_thread::get_id()) return true;
			Fail(state, "frame observation used from a non-owner thread");
			return false;
		}
		bool SameFrame(const engine::core::CFrame &left, const engine::core::CFrame &right) {
			const auto &a = left.Rotation();
			const auto &b = right.Rotation();
			return left.Position.X == right.Position.X && left.Position.Y == right.Position.Y &&
				   left.Position.Z == right.Position.Z && a.x == b.x && a.y == b.y && a.z == b.z &&
				   a.w == b.w;
		}
		bool SameCamera(const engine::scene::Camera &left, const engine::scene::Camera &right) {
			return left.FieldOfViewRadians == right.FieldOfViewRadians && left.NearPlane == right.NearPlane &&
				   left.FarPlane == right.FarPlane && left.MaxImageWidth == right.MaxImageWidth &&
				   left.MaxImageHeight == right.MaxImageHeight && left.ImageWidth == right.ImageWidth &&
				   left.ImageHeight == right.ImageHeight &&
				   left.RenderFeatures.Enable == right.RenderFeatures.Enable &&
				   left.RenderFeatures.Disable == right.RenderFeatures.Disable;
		}
		// Charge container nodes conservatively, including JSON tree and string capacity.
		size_t JsonBytes(const nlohmann::json &value) {
			size_t bytes = 128;
			if (value.is_string()) return bytes + value.get_ref<const std::string &>().capacity();
			if (value.is_array())
				bytes += value.get_ref<const nlohmann::json::array_t &>().capacity() * sizeof(nlohmann::json);
			if (value.is_structured())
				for (auto entry = value.begin(); entry != value.end(); ++entry) {
					if (value.is_object()) bytes += entry.key().capacity();
					bytes += JsonBytes(entry.value());
				}
			return bytes;
		}
		void DropPending(const std::shared_ptr<FrameObservationState> &state) {
			if (!state->Pending) return;
			state->Pending.reset();
			state->Bytes -= state->PendingBytes;
			state->Frames--;
			state->PendingBytes = 0;
			state->PendingToken = 0;
			state->PendingRenderer = nullptr;
		}
		void FinishSubscription(const std::shared_ptr<FrameObservationState> &state, bool validate) {
			if (state->Finished) return;
			if (validate &&
				(state->Delivered == 0 || (!state->Configuration.CaptureFrames.empty() &&
										   state->Delivered != state->Configuration.CaptureFrames.size())))
				Fail(state, "required client frame observations were not completed");
			if (state->PendingRenderer) {
				(void)state->PendingRenderer->CancelSceneCapture(state->PendingToken);
				Fail(state, "frame observation cancelled before completion");
				DropPending(state);
			}
			state->Closed = true;
			state->Finished = true;
			state->Callback = {};
			auto &registry = Subscriptions();
			std::lock_guard lock(registry.Mutex);
			const auto found = registry.Clients.find(state->Identity);
			if (found != registry.Clients.end() && found->second.Subscription == state)
				found->second.Subscription.reset();
		}
	}

	std::vector<FrameObjectObservation> CopyFrameObjects(const engine::render::View &view) {
		std::vector<FrameObjectObservation> objects;
		objects.reserve(view.Instances.size());
		for (size_t index = 0; index < view.Instances.size(); ++index) {
			const auto &row = view.Instances[index];
			FrameObjectObservation object;
			object.World = view.WorldName.Text();
			const bool ownWorld = !row.SourceWorld.IsValid() || row.SourceWorld == view.WorldName;
			object.SourceWorld = (row.SourceWorld.IsValid() ? row.SourceWorld : view.WorldName).Text();
			object.Object = std::to_string(row.Source);
			object.Mesh = row.Mesh.Text();
			object.Shape = row.Mesh.IsValid() ? object.Mesh : "cube";
			object.Frame = row.Frame;
			object.HalfExtent = row.HalfExtent;
			object.Tint = row.Tint;
			object.Rig = row.Rig;
			object.Source = row.Source;
			object.Variant = row.Variant;
			object.BodyKeyHigh = row.BodyKeyHigh;
			object.BodyKeyLow = row.BodyKeyLow;
			object.BodyGeneration = row.BodyGeneration;
			object.SeamNormal = row.SeamNormal;
			object.SeamOffset = row.SeamOffset;
			object.SeamFirst = row.SeamFirst;
			object.SeamSecond = row.SeamSecond;
			object.SeamCentre = row.SeamCentre;
			object.SeamMask = row.SeamMask;
			object.DrawIndex = index;
			// Keep the renderer's source-world and variant gates: rig handles collide between worlds.
			object.HiddenForEye =
				(view.EyeRig != 0 && ownWorld && row.Variant == 0 &&
				 (row.Rig == view.EyeRig || row.Source == view.EyeRig)) ||
				std::binary_search(view.EyeHiddenRows.begin(), view.EyeHiddenRows.end(), index);
			objects.push_back(std::move(object));
		}
		return objects;
	}

	FrameObservationLease::~FrameObservationLease() {
		Reset();
	}
	FrameObservationLease::FrameObservationLease(FrameObservationLease &&other) noexcept
		: State(std::move(other.State)) {}
	FrameObservationLease &FrameObservationLease::operator=(FrameObservationLease &&other) noexcept {
		if (this != &other) {
			Reset();
			State = std::move(other.State);
		}
		return *this;
	}
	FrameObservationLease::operator bool() const {
		return State && !State->Closed && Failure().empty();
	}
	std::string FrameObservationLease::Failure() const {
		if (!State) return "empty frame observation lease";
		std::lock_guard lock(State->FailureMutex);
		return State->Error;
	}
	FrameObservationOptions FrameObservationLease::Options() const {
		return State ? State->Configuration : FrameObservationOptions{};
	}
	uint64_t FrameObservationLease::Generation() const {
		return State ? State->Generation : 0;
	}
	uint64_t FrameObservationLease::CapturedFrames() const {
		return State ? State->Delivered.load() : 0;
	}
	size_t FrameObservationLease::OwnedBytes() const {
		return State ? State->Bytes.load() : 0;
	}
	bool FrameObservationLease::Finish() {
		if (!State || !OnOwner(State)) return false;
		FinishSubscription(State, true);
		return Failure().empty();
	}
	void FrameObservationLease::Reset() {
		if (!State || !OnOwner(State)) return;
		FinishSubscription(State, State->Started);
	}
	FrameObservationLease SubscribeClientFrames(
		Client &client, FrameObservationOptions options, FrameObservationCallback callback
	) {
		FrameObservationLease lease;
		lease.State = std::make_shared<FrameObservationState>();
		auto &state = lease.State;
		state->Identity = &client;
		state->Owner = std::this_thread::get_id();
		state->Configuration = options;
		std::sort(state->Configuration.CaptureFrames.begin(), state->Configuration.CaptureFrames.end());
		state->Callback = std::move(callback);
		auto &registry = Subscriptions();
		std::lock_guard lock(registry.Mutex);
		auto [entry, inserted] =
			registry.Clients.try_emplace(&client, ClientLifetime{state->Owner, false, {}});
		(void)inserted;
		if (entry->second.Owner != state->Owner)
			Fail(state, "frame subscription belongs to another thread");
		else if (entry->second.Started)
			Fail(state, "subscribe before Client::Initialise");
		else if (entry->second.Subscription)
			Fail(state, "duplicate frame subscription");
		else if (std::adjacent_find(
					 state->Configuration.CaptureFrames.begin(), state->Configuration.CaptureFrames.end()
				 ) != state->Configuration.CaptureFrames.end())
			Fail(state, "duplicate selected client frame");
		else if (!options.MaxPendingFrames || !options.MaxOwnedBytes || !state->Callback)
			Fail(state, "invalid frame observation configuration");
		else {
			state->Generation = registry.NextGeneration++;
			entry->second.Subscription = state;
		}
		return lease;
	}
	void FailClientFrameObservation(const Client &client, std::string error) {
		if (const auto state = Subscription(client)) Fail(state, std::move(error));
	}
	uint64_t ClientFrameObservationGeneration(const Client &client) {
		const auto state = Subscription(client);
		return state ? state->Generation : 0;
	}
	bool HasClientFrameSubscription(const Client &client) {
		const auto state = Subscription(client);
		return state && !state->Closed;
	}
	bool ShouldObserveClientFrame(const Client &client, uint64_t frame) {
		const auto state = Subscription(client);
		if (!state || state->Closed) return false;
		const auto &frames = state->Configuration.CaptureFrames;
		return frames.empty() || std::find(frames.begin(), frames.end(), frame) != frames.end();
	}
	bool ObservationEnabled(const Client &client, const std::filesystem::path &captureSequence) {
		return !captureSequence.empty() || HasClientFrameSubscription(client);
	}
	bool BeginClientFrameObservation(Client &client) {
		auto &registry = Subscriptions();
		std::lock_guard lock(registry.Mutex);
		auto [entry, inserted] =
			registry.Clients.try_emplace(&client, ClientLifetime{std::this_thread::get_id(), false, {}});
		(void)inserted;
		if (entry->second.Owner != std::this_thread::get_id() || entry->second.Started) {
			if (entry->second.Subscription)
				Fail(
					entry->second.Subscription,
					"client observation lifecycle started twice or from another thread"
				);
			return false;
		}
		entry->second.Started = true;
		if (entry->second.Subscription) entry->second.Subscription->Started = true;
		return true;
	}
	void CloseClientFrameObservation(Client &client, engine::render::Renderer &) {
		const auto state = Subscription(client);
		if (state && OnOwner(state)) FinishSubscription(state, state->Started);
		auto &registry = Subscriptions();
		std::lock_guard lock(registry.Mutex);
		registry.Clients.erase(&client);
	}
	void PrepareClientFrameObservation(
		Client &client,
		engine::render::Renderer &renderer,
		const engine::render::View &view,
		uint64_t clientFrame,
		nlohmann::json &&metadata,
		std::vector<FrameObjectObservation> &&objects
	) {
		const auto state = Subscription(client);
		if (!state || state->Closed || !OnOwner(state) || !ShouldObserveClientFrame(client, clientFrame))
			return;
		try {
			if (state->Pending || state->Frames >= state->Configuration.MaxPendingFrames) {
				Fail(state, "frame observation pending-frame budget exceeded");
				return;
			}
			size_t bytes = sizeof(FrameObservation) + JsonBytes(metadata) +
						   objects.capacity() * sizeof(FrameObjectObservation);
			for (const auto &object : objects)
				bytes += object.World.capacity() + object.SourceWorld.capacity() + object.Object.capacity() +
						 object.Mesh.capacity() + object.Shape.capacity();
			const size_t available = state->Configuration.MaxOwnedBytes - state->Bytes.load();
			if (bytes >= available) {
				Fail(state, "frame observation owned-byte budget exceeded");
				return;
			}
			auto observation = std::make_unique<FrameObservation>();
			observation->Generation = state->Generation;
			observation->ClientFrame = clientFrame;
			observation->Image.ViewSlot = view.Slot;
			observation->Image.World = view.World;
			observation->Image.WorldName = view.WorldName.Text();
			observation->Image.SnapshotId = view.SnapshotId;
			observation->Image.CameraFrame = view.CameraFrame;
			observation->Image.Camera = view.Camera;
			if (!view.Target || !view.Target->IsValid()) {
				Fail(state, "observed native view has no owned scene target");
				return;
			}
			observation->Image.Width = view.Target->Width;
			observation->Image.Height = view.Target->Height;
			if (!view.Projection) {
				float nearestPane = std::numeric_limits<float>::infinity();
				for (const auto &portal : view.Portals)
					nearestPane = std::min(
						nearestPane,
						engine::scene::RectangleDistance(
							portal.Centre, portal.First, portal.Second, view.CameraFrame.Position
						)
					);
				observation->Image.Camera.NearPlane =
					engine::scene::PortalNearPlane(view.Camera.NearPlane, nearestPane);
			}
			observation->Image.Projection =
				view.Projection
					? *view.Projection
					: engine::scene::ResolveCamera(
						  view.CameraFrame,
						  observation->Image.Camera,
						  static_cast<float>(view.Target->Width) / static_cast<float>(view.Target->Height)
					  )
						  .Projection;
			const auto pipeline = renderer.ResolvePipelineIdentity(view.Pipeline);
			if (!pipeline) {
				Fail(state, "observed native pipeline has no identity");
				return;
			}
			observation->Image.Pipeline = pipeline->Name;
			observation->Image.PipelineRevision = pipeline->Revision;
			bytes += observation->Image.WorldName.capacity() + observation->Image.SnapshotId.capacity();
			if (bytes >= available) {
				Fail(state, "frame observation owned-byte budget exceeded");
				return;
			}
			if (metadata.value("frame", uint64_t(-1)) != clientFrame ||
				metadata.value("observation_generation", uint64_t{0}) != state->Generation) {
				Fail(state, "frame metadata identity does not match its native capture request");
				return;
			}
			observation->Metadata = std::move(metadata);
			observation->Objects = std::move(objects);
			const uint64_t token = renderer.RequestSceneCapture(
				engine::render::SceneCaptureRequest{
					view.Slot, available - bytes, std::string(view.SnapshotId)
				}
			);
			if (!token) {
				Fail(state, "renderer refused owned frame capture");
				return;
			}
			state->Pending = std::move(observation);
			state->PendingToken = token;
			state->PendingRenderer = &renderer;
			state->PendingBytes = bytes;
			state->Bytes += bytes;
			state->Frames++;
		} catch (const std::exception &error) {
			Fail(state, std::string("frame observation copy failed: ") + error.what());
		}
	}
	void CompleteClientFrameObservation(Client &client, engine::render::Renderer &renderer, bool submitted) {
		const auto state = Subscription(client);
		if (!state || !OnOwner(state) || !state->Pending) return;
		try {
			auto capture = renderer.TakeSceneCapture(state->PendingToken);
			if (!submitted || state->Closed || !capture ||
				capture->Status != engine::render::SceneCaptureStatus::Ready ||
				capture->Token != state->PendingToken ||
				capture->ViewSlot != state->Pending->Image.ViewSlot ||
				capture->World != state->Pending->Image.World ||
				capture->WorldName != state->Pending->Image.WorldName ||
				capture->SnapshotId != state->Pending->Image.SnapshotId ||
				capture->Frame <= state->LastRendererFrame ||
				(state->LastClientFrame && state->Pending->ClientFrame <= *state->LastClientFrame) ||
				!SameFrame(capture->CameraFrame, state->Pending->Image.CameraFrame) ||
				!SameCamera(capture->Camera, state->Pending->Image.Camera) ||
				capture->Projection != state->Pending->Image.Projection ||
				capture->Width != state->Pending->Image.Width ||
				capture->Height != state->Pending->Image.Height ||
				capture->Pipeline != state->Pending->Image.Pipeline ||
				capture->PipelineRevision != state->Pending->Image.PipelineRevision) {
				(void)renderer.CancelSceneCapture(state->PendingToken);
				Fail(
					state,
					!submitted ? "observed frame was not submitted" : "owned frame capture did not complete"
				);
				DropPending(state);
				return;
			}
			state->LastRendererFrame = capture->Frame;
			state->LastClientFrame = state->Pending->ClientFrame;
			state->Pending->Pipeline = capture->Pipeline.Text();
			const size_t imageBytes = capture->Pixels.capacity() + capture->WorldName.capacity() +
									  capture->SnapshotId.capacity() + state->Pending->Pipeline.capacity();
			if (imageBytes > state->Configuration.MaxOwnedBytes - state->Bytes.load()) {
				Fail(state, "completed frame exceeded owned-byte budget");
				DropPending(state);
				return;
			}
			state->Bytes += imageBytes;
			const size_t ownedBytes = state->PendingBytes + imageBytes;
			state->Pending->Image = std::move(*capture);
			state->Pending->OwnedBytes = ownedBytes;
			auto immutable = std::shared_ptr<const FrameObservation>(
				state->Pending.release(), [state, ownedBytes](const FrameObservation *observation) {
					delete observation;
					state->Bytes -= ownedBytes;
					state->Frames--;
				}
			);
			state->PendingToken = 0;
			state->PendingRenderer = nullptr;
			state->PendingBytes = 0;
			try {
				const auto callback = state->Callback;
				callback(std::move(immutable));
				state->Delivered++;
			} catch (const std::exception &error) {
				Fail(state, std::string("frame observation callback failed: ") + error.what());
			} catch (...) {
				Fail(state, "frame observation callback threw a non-standard exception");
			}
		} catch (const std::exception &error) {
			if (state->PendingToken) (void)renderer.CancelSceneCapture(state->PendingToken);
			Fail(state, std::string("completed frame copy failed: ") + error.what());
			DropPending(state);
		} catch (...) {
			// Owned transfer failures must reach the calling test rather than the frame loop.
			if (state->PendingToken) (void)renderer.CancelSceneCapture(state->PendingToken);
			Fail(state, "completed frame copy threw a non-standard exception");
			DropPending(state);
		}
	}
}
