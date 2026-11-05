#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/SpatialInputQueue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

TEST_SUITE_ID("engine.render.spatialinputqueue")

using namespace engine;
using namespace engine::render;

namespace {
	struct DiscardLog {
		struct Entry {
			uint64_t Host = 0, Sequence = 0;
			std::byte Value{};
			SpatialInputQueue::DiscardReason Reason{};
		};
		std::array<Entry, 16> Entries{};
		size_t Count = 0;
	};
	thread_local DiscardLog *CurrentDiscards = nullptr;
	void CaptureDiscard(
		uint64_t host,
		uint64_t sequence,
		std::span<const std::byte> payload,
		SpatialInputQueue::DiscardReason reason
	) {
		if (CurrentDiscards && CurrentDiscards->Count < CurrentDiscards->Entries.size()) {
			auto &entry = CurrentDiscards->Entries[CurrentDiscards->Count++];
			entry = {host, sequence, payload.empty() ? std::byte{} : payload.front(), reason};
		}
	}
	void ThrowDiscard(uint64_t, uint64_t, std::span<const std::byte>, SpatialInputQueue::DiscardReason) {
		throw 1;
	}
	struct DiscardCapture {
		DiscardLog Log;
		DiscardCapture() {
			CurrentDiscards = &Log;
		}
		~DiscardCapture() {
			CurrentDiscards = nullptr;
		}
	};

	struct FakeDepth : SpatialInputDepth {
		SpatialDepthFrameStatus FrameStatus = SpatialDepthFrameStatus::Unknown;
		SpatialDepthReadbackStatus ReadStatus = SpatialDepthReadbackStatus::Pending;
		SpatialDepthFrameInfo Frame;
		float Depth = 1.0f;
		int Queued = 0, Cancelled = 0;
		bool CanCancel = true;
		SpatialDepthFrameStatus Status(SpatialDepthFrameKey) override {
			return FrameStatus;
		}
		std::optional<SpatialDepthFrameInfo> Info(SpatialDepthFrameKey) override {
			return Frame;
		}
		uint64_t Queue(SpatialDepthFrameKey, uint32_t, uint32_t) override {
			++Queued;
			return 123;
		}
		SpatialDepthReadbackResult Take(SpatialDepthFrameKey key, uint64_t) override {
			return {ReadStatus, key, Depth};
		}
		bool Cancel(SpatialDepthFrameKey, uint64_t) override {
			++Cancelled;
			return CanCancel;
		}
	};

	gui::PointerFrame MakeScreenFrame() {
		gui::RegisterGuiClasses();
		ecs::Store store("queue fixture");
		const auto collector = store.CreateInstance(gui::GuiClass("ScreenGui"), "collector");
		const auto button = store.CreateInstance(gui::GuiClass("TextButton"), "button");
		store.SetParent(button, collector);
		gui::DrawList list;
		list.CanvasSize = {800, 600};
		gui::DrawCommand command;
		command.Source = button;
		command.Collector = collector;
		command.Bounds = {{0, 0}, {100, 100}};
		command.Clip = command.Bounds;
		list.Commands.push_back(command);
		gui::Router router;
		return router.CapturePointerFrame(store, list);
	}

	gui::PointerFrame MakeSpatialFrame(bool interactive = true) {
		gui::RegisterGuiClasses();
		ecs::Store store("spatial queue fixture");
		const auto collector = store.CreateInstance(gui::GuiClass("SurfaceGui"), "collector");
		const auto button = store.CreateInstance(gui::GuiClass("TextButton"), "button");
		store.SetParent(button, collector);
		gui::SpatialCanvas canvas;
		canvas.Origin = {-1, -1, .5f};
		canvas.AxisX = {2, 0, 0};
		canvas.AxisY = {0, 2, 0};
		canvas.Normal = {0, 0, -1};
		canvas.Size = {100, 100};
		canvas.Interactive = interactive;
		store.Set<gui::SpatialCanvas>(collector, canvas);
		gui::DrawList list;
		list.CanvasSize = {800, 600};
		gui::DrawCommand command;
		command.Source = button;
		command.Collector = collector;
		command.Bounds = {{0, 0}, {100, 100}};
		command.Clip = command.Bounds;
		list.Commands.push_back(command);
		gui::Router router;
		return router.CapturePointerFrame(store, list);
	}

}
TEST_CASE("cancelled opaque events report each discarded payload", "[render][spatialqueue]") {
	DiscardCapture captured;
	SpatialInputQueue queue;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	const auto store = frame.StoreIdentity();
	REQUIRE(queue.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	REQUIRE(queue.Publish(presented, 7));
	const std::array first{std::byte{1}}, second{std::byte{2}};
	REQUIRE(queue.EnqueueOpaque(1, 41, first, false, 0, CaptureDiscard));
	REQUIRE(queue.EnqueueOpaque(1, 41, second, false, 0, CaptureDiscard));
	REQUIRE(queue.SealBatch(41, store));
	REQUIRE(queue.RequestCancel());
	const auto result = queue.Poll(depth);
	REQUIRE(result.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(captured.Log.Count == 2);
	CHECK(captured.Log.Entries[0].Value == std::byte{1});
	CHECK(captured.Log.Entries[1].Value == std::byte{2});
	CHECK(captured.Log.Entries[0].Host == 41);
	CHECK(captured.Log.Entries[0].Sequence == 1);
	CHECK(captured.Log.Entries[0].Reason == SpatialInputQueue::DiscardReason::Cancellation);
	REQUIRE(queue.Acknowledge(result.Event));
	REQUIRE(queue.Reset(depth));
	CHECK(captured.Log.Count == 2);
}

TEST_CASE("only discarded opaque events notify their observer", "[render][spatialqueue]") {
	DiscardCapture captured;
	FakeDepth depth;
	{
		SpatialInputQueue queue;
		auto frame = MakeScreenFrame();
		const auto store = frame.StoreIdentity();
		REQUIRE(queue.Stage(1, std::move(frame), 7, 41, {}));
		FrameResult presented;
		presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
		presented.SpatialSnapshotSequence = 1;
		REQUIRE(queue.Publish(presented, 7));
		const std::array bytes{std::byte{3}};
		REQUIRE(queue.EnqueueOpaque(1, 41, bytes, false, 0, CaptureDiscard));
		REQUIRE(queue.EnqueueOpaque(1, 41, bytes, false, 0, CaptureDiscard));
		REQUIRE(queue.SealBatch(41, store));
		REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Ready);
		const auto delivered = queue.Poll(depth);
		REQUIRE(delivered.State == SpatialInputQueue::Status::Ready);
		REQUIRE(queue.Acknowledge(delivered.Event));
		CHECK(captured.Log.Count == 0);
		const auto borrowed = queue.Poll(depth);
		REQUIRE(borrowed.State == SpatialInputQueue::Status::Ready);
		REQUIRE(queue.DiscardBorrowed());
		REQUIRE(captured.Log.Count == 1);
		CHECK(captured.Log.Entries[0].Reason == SpatialInputQueue::DiscardReason::Borrowed);
		REQUIRE(queue.EnqueueOpaque(1, 41, bytes, false, 0, CaptureDiscard));
	}
	REQUIRE(captured.Log.Count == 2);
	CHECK(captured.Log.Entries[1].Reason == SpatialInputQueue::DiscardReason::Destruction);
}

TEST_CASE("a throwing discard observer does not prevent later reports", "[render][spatialqueue]") {
	DiscardCapture captured;
	SpatialInputQueue queue;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	const auto store = frame.StoreIdentity();
	REQUIRE(queue.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	REQUIRE(queue.Publish(presented, 7));
	const std::array bytes{std::byte{8}};
	REQUIRE(queue.EnqueueOpaque(1, 41, bytes, false, 0, ThrowDiscard));
	REQUIRE(queue.EnqueueOpaque(1, 41, bytes, false, 0, CaptureDiscard));
	REQUIRE(queue.SealBatch(41, store));
	REQUIRE(queue.RequestCancel());
	const auto result = queue.Poll(depth);
	REQUIRE(result.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(captured.Log.Count == 1);
	CHECK(captured.Log.Entries[0].Value == std::byte{8});
	REQUIRE(queue.Acknowledge(result.Event));
}

TEST_CASE("sealed batches preserve host barriers", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(presented, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	const std::array bytes{std::byte{2}};
	REQUIRE(q.EnqueueOpaque(1, 41, bytes));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto first = q.Poll(depth);
	REQUIRE(first.State == SpatialInputQueue::Status::Ready);
	CHECK(q.Acknowledge(first.Event));
	auto end = q.Poll(depth);
	REQUIRE(end.State == SpatialInputQueue::Status::BatchEnd);
	CHECK(end.Batch == first.Batch);
	CHECK(q.Acknowledge(end.Event));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto next = q.Poll(depth);
	REQUIRE(next.State == SpatialInputQueue::Status::Ready);
	CHECK(next.Type == SpatialInputQueue::Kind::Opaque);
	CHECK(next.Batch != first.Batch);
}
TEST_CASE("activation waits for physical release", "[render][spatialqueue]") {
	SpatialInputQueue q;
	REQUIRE(q.BeginPhysicalTracking(true));
	CHECK(q.SuppressingContact());
	q.ObservePhysicalPrimary(true);
	CHECK(q.SuppressingContact());
	q.ObservePhysicalPrimary(false);
	CHECK_FALSE(q.SuppressingContact());
}
TEST_CASE("retiring old host preserves replacement frame", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto old = MakeScreenFrame();
	auto fresh = MakeScreenFrame();
	auto id = fresh.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(old), 7, 41, {}));
	REQUIRE(q.Stage(2, std::move(fresh), 7, 42, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 2;
	REQUIRE(q.Publish(p, 7));
	REQUIRE(q.RetireHost(depth, 41, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	REQUIRE(q.EnqueuePointer(2, {}, pointer, 42));
	REQUIRE(q.SealBatch(42, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	CHECK(q.Poll(depth).State == SpatialInputQueue::Status::Ready);
}
TEST_CASE("preflight waits for scene depth before a batch starts", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeSpatialFrame();
	auto id = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(frame), 7, 12, screen));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.Frame.Key = {1, id, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	presented.SpatialDepth = depth.Frame.Key;
	REQUIRE(q.Publish(presented, 7));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	REQUIRE(q.EnqueuePointer(1, screen, pointer, 12));
	REQUIRE(q.SealBatch(12, id));
	CHECK(q.Preflight(depth) == SpatialInputQueue::Status::Pending);
	CHECK(depth.Queued == 1);
	CHECK(q.Poll(depth).State == SpatialInputQueue::Status::Pending);
	depth.ReadStatus = SpatialDepthReadbackStatus::Ready;
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto result = q.Poll(depth);
	REQUIRE(result.State == SpatialInputQueue::Status::Ready);
	CHECK(result.Pointer.Collector != ecs::NULL_ENTITY);
}
TEST_CASE("invalid resize cancels an already held contact until physical up", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto press = q.Poll(depth);
	REQUIRE(q.Acknowledge(press.Event));
	auto end = q.Poll(depth);
	REQUIRE(q.Acknowledge(end.Event));
	gui::Screen resized;
	resized.Width += 1;
	CHECK_FALSE(q.EnqueuePointer(1, resized, pointer, 41));
	auto cancelled = q.Poll(depth);
	REQUIRE(cancelled.State == SpatialInputQueue::Status::Cancelled);
	CHECK(cancelled.Pointer.Down);
	CHECK(q.Acknowledge(cancelled.Event));
	CHECK(q.SuppressingContact());
	q.ObservePhysicalPrimary(false);
	auto release = q.Poll(depth);
	REQUIRE(release.State == SpatialInputQueue::Status::Cancelled);
	CHECK_FALSE(release.Pointer.Down);
	CHECK(q.Acknowledge(release.Event));
	CHECK_FALSE(q.SuppressingContact());
}
TEST_CASE("interleaved hosts seal only their contiguous events", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto a = MakeScreenFrame();
	auto b = MakeScreenFrame();
	auto aid = a.StoreIdentity();
	auto bid = b.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(a), 7, 41, {}));
	REQUIRE(q.Stage(2, std::move(b), 8, 42, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	p.SpatialSnapshotSequence = 2;
	REQUIRE(q.Publish(p, 8));
	const std::array bytes{std::byte{1}};
	REQUIRE(q.EnqueueOpaque(1, 41, bytes));
	REQUIRE(q.EnqueueOpaque(2, 42, bytes));
	REQUIRE(q.EnqueueOpaque(1, 41, bytes));
	REQUIRE(q.SealBatch(41, aid));
	REQUIRE(q.SealBatch(42, bid));
	REQUIRE(q.SealBatch(41, aid));
	for (const auto host : {41u, 42u, 41u}) {
		REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
		auto event = q.Poll(depth);
		REQUIRE(event.State == SpatialInputQueue::Status::Ready);
		CHECK(event.HostToken == host);
		REQUIRE(q.Acknowledge(event.Event));
		auto end = q.Poll(depth);
		REQUIRE(end.State == SpatialInputQueue::Status::BatchEnd);
		REQUIRE(q.Acknowledge(end.Event));
	}
}
TEST_CASE("host retirement retries cancellation and preserves other hosts", "[render][spatialqueue]") {
	DiscardCapture captured;
	SpatialInputQueue q;
	FakeDepth depth;
	auto old = MakeSpatialFrame();
	auto fresh = MakeScreenFrame();
	auto oid = old.StoreIdentity();
	auto fid = fresh.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(old), 7, 41, screen));
	REQUIRE(q.Stage(2, std::move(fresh), 7, 42, {}));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.Frame.Key = {1, oid, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	p.SpatialDepth = depth.Frame.Key;
	REQUIRE(q.Publish(p, 7));
	p.SpatialSnapshotSequence = 2;
	p.SpatialDepth = {};
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	REQUIRE(q.EnqueuePointer(1, screen, pointer, 41));
	const std::array bytes{std::byte{3}};
	REQUIRE(q.EnqueueOpaque(1, 41, bytes, false, 0, CaptureDiscard));
	REQUIRE(q.SealBatch(41, oid));
	REQUIRE(q.EnqueueOpaque(2, 42, bytes));
	REQUIRE(q.SealBatch(42, fid));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Pending);
	depth.CanCancel = false;
	CHECK_FALSE(q.RetireHost(depth, 41, 7));
	CHECK(captured.Log.Count == 0);
	depth.CanCancel = true;
	REQUIRE(q.RetireHost(depth, 41, 7));
	REQUIRE(captured.Log.Count == 1);
	CHECK(captured.Log.Entries[0].Reason == SpatialInputQueue::DiscardReason::HostRetired);
	CHECK(depth.Cancelled == 2);
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto event = q.Poll(depth);
	REQUIRE(event.State == SpatialInputQueue::Status::Ready);
	CHECK(event.HostToken == 42);
}
TEST_CASE("canceled contact cannot restart from later queued samples", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	pointer.Down = false;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto press = q.Poll(depth);
	REQUIRE(press.State == SpatialInputQueue::Status::Ready);
	REQUIRE(q.Acknowledge(press.Event));
	REQUIRE(q.RequestCancel());
	auto cancel = q.Poll(depth);
	REQUIRE(cancel.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(q.Acknowledge(cancel.Event));
	auto end = q.Poll(depth);
	REQUIRE(end.State == SpatialInputQueue::Status::BatchEnd);
	REQUIRE(q.Acknowledge(end.Event));
	q.ObservePhysicalPrimary(false);
	auto release = q.Poll(depth);
	REQUIRE(release.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(q.Acknowledge(release.Event));
	for (int i = 0; i < 2; ++i) {
		REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
		auto skipped = q.Poll(depth);
		REQUIRE(skipped.State == SpatialInputQueue::Status::BatchEnd);
		REQUIRE(q.Acknowledge(skipped.Event));
	}
	q.ObservePhysicalPrimary(true);
	pointer.Down = true;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	CHECK(q.Poll(depth).State == SpatialInputQueue::Status::Ready);
}
TEST_CASE("failed opaque prefix revokes discarded later contact", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	const std::array bytes{std::byte{1}};
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	REQUIRE(q.EnqueueOpaque(1, 41, bytes));
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.RequestCancel());
	auto cancel = q.Poll(depth);
	REQUIRE(cancel.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(q.Acknowledge(cancel.Event));
	auto end = q.Poll(depth);
	REQUIRE(end.State == SpatialInputQueue::Status::BatchEnd);
	REQUIRE(q.Acknowledge(end.Event));
	q.ObservePhysicalPrimary(false);
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto skipped = q.Poll(depth);
	REQUIRE(skipped.State == SpatialInputQueue::Status::BatchEnd);
}
TEST_CASE("a newer contact survives cancellation of an older one", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	q.ObservePhysicalPrimary(false);
	pointer.Down = false;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	q.ObservePhysicalPrimary(true);
	pointer.Down = true;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.RequestCancel());
	auto cancel = q.Poll(depth);
	REQUIRE(cancel.State == SpatialInputQueue::Status::Cancelled);
	CHECK_FALSE(cancel.Pointer.Down);
	REQUIRE(q.Acknowledge(cancel.Event));
	auto end = q.Poll(depth);
	REQUIRE(q.Acknowledge(end.Event));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	end = q.Poll(depth);
	REQUIRE(end.State == SpatialInputQueue::Status::BatchEnd);
	REQUIRE(q.Acknowledge(end.Event));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto fresh = q.Poll(depth);
	REQUIRE(fresh.State == SpatialInputQueue::Status::Ready);
	REQUIRE(q.Acknowledge(fresh.Event));
	end = q.Poll(depth);
	REQUIRE(q.Acknowledge(end.Event));
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	q.ObservePhysicalPrimary(false);
	pointer.Down = false;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto move = q.Poll(depth);
	REQUIRE(move.State == SpatialInputQueue::Status::Ready);
	CHECK(move.Pointer.Down);
	REQUIRE(q.Acknowledge(move.Event));
	auto up = q.Poll(depth);
	REQUIRE(up.State == SpatialInputQueue::Status::Ready);
	CHECK_FALSE(up.Pointer.Down);
}
TEST_CASE("frame metadata is owned and available only once published", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	std::array bytes{std::byte{1}, std::byte{2}};
	SpatialInputQueue::ResolverView view;
	view.Camera.FieldOfViewRadians = 0.8f;
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}, view, bytes));
	bytes[0] = std::byte{9};
	CHECK_FALSE(q.PublishedMetadata(1).has_value());
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	auto meta = q.PublishedMetadata(1);
	REQUIRE(meta);
	REQUIRE(meta->size() == 2);
	CHECK((*meta)[0] == std::byte{1});
	REQUIRE(q.EnqueueOpaque(1, 41, {}));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto result = q.Poll(depth);
	REQUIRE(result.Resolver);
	CHECK(result.Resolver->Camera.FieldOfViewRadians == 0.8f);
	CHECK(result.FrameMetadata[0] == std::byte{1});
}
TEST_CASE("ordinary opaque input does not wait for scene depth", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeSpatialFrame();
	auto id = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, screen));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	p.SpatialDepth = {1, id, 7, 9};
	REQUIRE(q.Publish(p, 7));
	const std::array bytes{std::byte{1}};
	REQUIRE(q.EnqueueOpaque(1, 41, bytes));
	REQUIRE(q.SealBatch(41, id));
	depth.FrameStatus = SpatialDepthFrameStatus::Pending;
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	CHECK(q.Poll(depth).State == SpatialInputQueue::Status::Ready);
	CHECK(depth.Queued == 0);
}
TEST_CASE("gesture dependent opaque records are revoked with their pointer", "[render][spatialqueue]") {
	DiscardCapture captured;
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	uint64_t generation = 0;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41, false, &generation));
	REQUIRE(generation != 0);
	REQUIRE(q.SealBatch(41, id));
	const std::array bytes{std::byte{1}};
	REQUIRE(q.EnqueueOpaque(1, 41, bytes, false, generation, CaptureDiscard));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.RequestCancel());
	auto cancel = q.Poll(depth);
	REQUIRE(cancel.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(q.Acknowledge(cancel.Event));
	auto end = q.Poll(depth);
	REQUIRE(q.Acknowledge(end.Event));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto skipped = q.Poll(depth);
	REQUIRE(skipped.State == SpatialInputQueue::Status::BatchEnd);
	REQUIRE(captured.Log.Count == 1);
	CHECK(captured.Log.Entries[0].Reason == SpatialInputQueue::DiscardReason::Revoked);
}
TEST_CASE("invalid scene depth cannot fall through", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeSpatialFrame();
	auto id = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, screen));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.ReadStatus = SpatialDepthReadbackStatus::Ready;
	depth.Frame.Key = {1, id, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	depth.Depth = 2.0f;
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	p.SpatialDepth = depth.Frame.Key;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	REQUIRE(q.EnqueuePointer(1, screen, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	CHECK(q.Poll(depth).State == SpatialInputQueue::Status::Cancelled);
}
TEST_CASE("depth preflight respects bounded slots and drains out of order", "[render][spatialqueue]") {
	struct SlotDepth final : FakeDepth {
		uint64_t Next = 1;
		std::vector<uint64_t> Started;
		bool Ready = false, All = false;
		uint64_t Queue(SpatialDepthFrameKey, uint32_t, uint32_t) override {
			Started.push_back(Next);
			return Next++;
		}
		SpatialDepthReadbackResult Take(SpatialDepthFrameKey key, uint64_t token) override {
			if (!Ready || (!All && token <= 8 && token % 2 == 1))
				return {SpatialDepthReadbackStatus::Pending, key, 0};
			return {SpatialDepthReadbackStatus::Ready, key, 0.8f};
		}
	} depth;
	SpatialInputQueue q;
	auto frame = MakeSpatialFrame();
	auto id = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, screen));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.Frame.Key = {1, id, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	p.SpatialDepth = depth.Frame.Key;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	for (int i = 0; i < 10; ++i)
		REQUIRE(q.EnqueuePointer(1, screen, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	CHECK(q.Preflight(depth) == SpatialInputQueue::Status::Pending);
	CHECK(depth.Started.size() == 8);
	depth.Ready = true;
	CHECK(q.Preflight(depth) == SpatialInputQueue::Status::Pending);
	CHECK(depth.Started.size() == 10);
	CHECK(q.Poll(depth).State == SpatialInputQueue::Status::Pending);
	depth.All = true;
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	for (int i = 0; i < 10; ++i) {
		auto item = q.Poll(depth);
		REQUIRE(item.State == SpatialInputQueue::Status::Ready);
		REQUIRE(q.Acknowledge(item.Event));
	}
}

TEST_CASE("preflight permits cancellation while readback is pending", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeSpatialFrame();
	auto id = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, screen));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.Frame.Key = {1, id, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	p.SpatialDepth = depth.Frame.Key;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	REQUIRE(q.EnqueuePointer(1, screen, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Pending);
	REQUIRE(q.RequestCancel());
	CHECK(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto cancelled = q.Poll(depth);
	REQUIRE(cancelled.State == SpatialInputQueue::Status::Cancelled);
	CHECK(depth.Cancelled == 1);
}
TEST_CASE("authoring selection ignores a runtime spatial capture", "[render][spatialqueue]") {
	SpatialInputQueue queue;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(queue.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	REQUIRE(queue.Publish(presented, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	REQUIRE(queue.EnqueuePointer(1, {}, pointer, 41, true));
	REQUIRE(queue.SealBatch(41, id));
	REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto result = queue.Poll(depth, ecs::Entity{123}, true);
	CHECK(result.State == SpatialInputQueue::Status::Ready);
	CHECK(result.Authoring);
	CHECK(result.Pointer.ScreenOnly);
}
TEST_CASE("authoring release belongs to its original canceled contact", "[render][spatialqueue]") {
	SpatialInputQueue queue;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(queue.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	REQUIRE(queue.Publish(presented, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	queue.ObservePhysicalPrimary(true);
	uint64_t generation = 0;
	REQUIRE(queue.EnqueuePointer(1, {}, pointer, 41, false, &generation));
	REQUIRE(generation);
	REQUIRE(queue.SealBatch(41, id));
	queue.ObservePhysicalPrimary(false);
	pointer.Down = false;
	REQUIRE(queue.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(queue.SealBatch(41, id));
	REQUIRE(queue.EnqueuePointer(1, {}, pointer, 41, true, nullptr, generation));
	REQUIRE(queue.SealBatch(41, id));
	REQUIRE(queue.RequestCancel());
	auto cancel = queue.Poll(depth);
	REQUIRE(cancel.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(queue.Acknowledge(cancel.Event));
	auto end = queue.Poll(depth);
	REQUIRE(queue.Acknowledge(end.Event));
	REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Ready);
	end = queue.Poll(depth);
	REQUIRE(end.State == SpatialInputQueue::Status::BatchEnd);
	REQUIRE(queue.Acknowledge(end.Event));
	REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Ready);
	CHECK(queue.Poll(depth).State == SpatialInputQueue::Status::BatchEnd);
}
TEST_CASE(
	"teardown discards only borrowed results and retries readback cancellation", "[render][spatialqueue]"
) {
	DiscardCapture captured;
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeSpatialFrame();
	auto id = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, screen));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.Frame.Key = {1, id, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	p.SpatialDepth = depth.Frame.Key;
	REQUIRE(q.Publish(p, 7));
	const std::array bytes{std::byte{1}};
	REQUIRE(q.EnqueueOpaque(1, 41, bytes));
	REQUIRE(q.SealBatch(41, id));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	REQUIRE(q.EnqueuePointer(1, screen, pointer, 41));
	REQUIRE(q.EnqueueOpaque(1, 41, bytes, false, 0, CaptureDiscard));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto ready = q.Poll(depth);
	REQUIRE(ready.State == SpatialInputQueue::Status::Ready);
	REQUIRE(q.DiscardBorrowed());
	auto end = q.Poll(depth);
	REQUIRE(end.State == SpatialInputQueue::Status::BatchEnd);
	REQUIRE(q.DiscardBorrowed());
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Pending);
	REQUIRE(q.DiscardBorrowed());
	depth.CanCancel = false;
	CHECK_FALSE(q.Reset(depth));
	CHECK(captured.Log.Count == 0);
	CHECK(depth.Cancelled == 1);
	depth.CanCancel = true;
	REQUIRE(q.Reset(depth));
	REQUIRE(captured.Log.Count == 1);
	CHECK(captured.Log.Entries[0].Reason == SpatialInputQueue::DiscardReason::Reset);
	CHECK(depth.Cancelled == 2);
	CHECK_FALSE(q.HasPending());
}
TEST_CASE("teardown discards a borrowed cancellation", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	REQUIRE(q.RequestCancel());
	auto result = q.Poll(depth);
	REQUIRE(result.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(q.DiscardBorrowed());
	CHECK_FALSE(q.HasPending());
	REQUIRE(q.Reset(depth));
}
TEST_CASE("an explicit held cancellation revokes later viewport samples", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto old = MakeScreenFrame();
	auto replacement = MakeScreenFrame();
	auto a = old.StoreIdentity();
	auto b = replacement.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(old), 7, 41, {}));
	REQUIRE(q.Stage(2, std::move(replacement), 8, 42, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	p.SpatialSnapshotSequence = 2;
	REQUIRE(q.Publish(p, 8));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	uint64_t generation = 0;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41, false, &generation));
	REQUIRE(q.SealBatch(41, a));
	pointer.Cancelled = true;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41, false, nullptr, generation));
	REQUIRE(q.SealBatch(41, a));
	pointer.Cancelled = false;
	REQUIRE(q.EnqueuePointer(2, {}, pointer, 42));
	REQUIRE(q.SealBatch(42, b));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto press = q.Poll(depth);
	REQUIRE(q.Acknowledge(press.Event));
	CHECK(q.HasContact());
	auto end = q.Poll(depth);
	REQUIRE(q.Acknowledge(end.Event));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto cancel = q.Poll(depth);
	REQUIRE(cancel.State == SpatialInputQueue::Status::Ready);
	REQUIRE(cancel.Pointer.Cancelled);
	REQUIRE(q.Acknowledge(cancel.Event));
	CHECK(q.SuppressingContact());
	end = q.Poll(depth);
	REQUIRE(q.Acknowledge(end.Event));
	q.ObservePhysicalPrimary(false);
	auto release = q.Poll(depth);
	REQUIRE(release.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(q.Acknowledge(release.Event));
	CHECK_FALSE(q.HasContact());
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	end = q.Poll(depth);
	CHECK(end.State == SpatialInputQueue::Status::BatchEnd);
}
TEST_CASE("authoring can select visible noninteractive spatial controls", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeSpatialFrame(false);
	auto id = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, screen));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.ReadStatus = SpatialDepthReadbackStatus::Ready;
	depth.Frame.Key = {1, id, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	p.SpatialDepth = depth.Frame.Key;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	REQUIRE(q.EnqueuePointer(1, screen, pointer, 41, true));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto selected = q.Poll(depth);
	REQUIRE(selected.State == SpatialInputQueue::Status::Ready);
	CHECK(selected.Authoring);
	CHECK_FALSE(selected.Pointer.ScreenOnly);
	CHECK(selected.Pointer.Collector != ecs::NULL_ENTITY);
}
TEST_CASE("explicit cancellation preserves a newer physical contact", "[render][spatialqueue]") {
	SpatialInputQueue q;
	FakeDepth depth;
	auto frame = MakeScreenFrame();
	auto id = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	gui::Pointer pointer;
	pointer.Position = {20, 20};
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	uint64_t generation = 0;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41, false, &generation));
	REQUIRE(q.SealBatch(41, id));
	pointer.Cancelled = true;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41, false, nullptr, generation));
	REQUIRE(q.SealBatch(41, id));
	pointer.Cancelled = false;
	pointer.Down = false;
	q.ObservePhysicalPrimary(false);
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	pointer.Down = true;
	q.ObservePhysicalPrimary(true);
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	for (int i = 0; i < 2; ++i) {
		REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
		auto event = q.Poll(depth);
		REQUIRE(event.State == SpatialInputQueue::Status::Ready);
		if (i == 1) {
			CHECK(event.Pointer.Cancelled);
			CHECK_FALSE(event.Pointer.Down);
		}
		REQUIRE(q.Acknowledge(event.Event));
		auto end = q.Poll(depth);
		REQUIRE(q.Acknowledge(end.Event));
	}
	CHECK_FALSE(q.SuppressingContact());
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto skipped = q.Poll(depth);
	REQUIRE(skipped.State == SpatialInputQueue::Status::BatchEnd);
	REQUIRE(q.Acknowledge(skipped.Event));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto fresh = q.Poll(depth);
	REQUIRE(fresh.State == SpatialInputQueue::Status::Ready);
	CHECK(fresh.Pointer.Down);
	REQUIRE(q.Acknowledge(fresh.Event));
	auto end = q.Poll(depth);
	REQUIRE(q.Acknowledge(end.Event));
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	q.ObservePhysicalPrimary(false);
	pointer.Down = false;
	REQUIRE(q.EnqueuePointer(1, {}, pointer, 41));
	REQUIRE(q.SealBatch(41, id));
	REQUIRE(q.Preflight(depth) == SpatialInputQueue::Status::Ready);
	auto moved = q.Poll(depth);
	REQUIRE(moved.State == SpatialInputQueue::Status::Ready);
	REQUIRE(q.Acknowledge(moved.Event));
	auto release = q.Poll(depth);
	REQUIRE(release.State == SpatialInputQueue::Status::Ready);
	CHECK_FALSE(release.Pointer.Down);
}
TEST_CASE("published frames can be borrowed for idle rebase", "[render][spatialqueue]") {
	SpatialInputQueue q;
	auto frame = MakeScreenFrame();
	auto store = frame.StoreIdentity();
	REQUIRE(q.Stage(1, std::move(frame), 7, 41, {}));
	CHECK(q.PublishedFrame(1) == nullptr);
	FrameResult p;
	p.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	p.SpatialSnapshotSequence = 1;
	REQUIRE(q.Publish(p, 7));
	const auto *retained = q.PublishedFrame(1);
	REQUIRE(retained);
	CHECK(retained->StoreIdentity() == store);
	CHECK(q.PublishedFrame(99) == nullptr);
}

TEST_CASE(
	"published projection survives depth eviction without bypassing occlusion", "[render][spatialqueue]"
) {
	SpatialInputQueue queue;
	FakeDepth depth;
	auto frame = MakeSpatialFrame();
	const auto store = frame.StoreIdentity();
	ecs::Entity collector;
	gui::SpatialCanvas canvas;
	REQUIRE(frame.SpatialCanvasAt(0, collector, canvas));
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(queue.Stage(1, std::move(frame), 7, 12, screen));
	depth.Frame.Key = {1, store, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	presented.SpatialDepth = depth.Frame.Key;
	presented.SpatialDepthProjection = depth.Frame;
	REQUIRE(queue.Publish(presented, 7));
	// Republishing the same sequence cannot replace its retained projection.
	presented.SpatialDepthProjection->Width = 5;
	REQUIRE(queue.Publish(presented, 7));
	presented.SpatialDepthProjection.reset();
	REQUIRE(queue.Publish(presented, 7));
	depth.FrameStatus = SpatialDepthFrameStatus::Unknown;
	SECTION("a held capture projects without retained depth") {
		gui::Pointer pointer;
		pointer.Position = {400, 300};
		pointer.Down = true;
		REQUIRE(queue.EnqueuePointer(1, screen, pointer, 12));
		REQUIRE(queue.SealBatch(12, store));
		REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Ready);
		const auto result = queue.Poll(depth, collector, true);
		REQUIRE(result.State == SpatialInputQueue::Status::Ready);
		CHECK(result.Pointer.Collector == collector);
		REQUIRE(result.FrameInfo);
		CHECK(result.FrameInfo->Key == depth.Frame.Key);
		CHECK(result.FrameInfo->Width == 800);
		CHECK(depth.Queued == 0);
	}
	SECTION("metadata-only input keeps its projection") {
		const std::array payload{std::byte{1}};
		REQUIRE(queue.EnqueueOpaque(1, 12, payload, true));
		REQUIRE(queue.SealBatch(12, store));
		REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Ready);
		const auto result = queue.Poll(depth);
		REQUIRE(result.State == SpatialInputQueue::Status::Ready);
		REQUIRE(result.FrameInfo);
		CHECK(result.FrameInfo->Key == depth.Frame.Key);
		CHECK(result.FrameInfo->Width == 800);
	}
	SECTION("a new spatial pick still requires depth") {
		gui::Pointer pointer;
		pointer.Position = {400, 300};
		REQUIRE(queue.EnqueuePointer(1, screen, pointer, 12));
		REQUIRE(queue.SealBatch(12, store));
		REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Ready);
		const auto result = queue.Poll(depth);
		CHECK(result.State == SpatialInputQueue::Status::Cancelled);
		CHECK(result.Error == SpatialInputQueue::Failure::DepthUnavailable);
	}
}

TEST_CASE("retained frame objects count toward the queue byte budget", "[render][spatialqueue]") {
	gui::RegisterGuiClasses();
	ecs::Store store("queue byte limit");
	const auto collector = store.CreateInstance(gui::GuiClass("ScreenGui"), "collector");
	const auto button = store.CreateInstance(gui::GuiClass("TextButton"), "button");
	store.SetParent(button, collector);
	gui::DrawList list;
	gui::DrawCommand command;
	command.Source = button;
	command.Collector = collector;
	command.Key.assign(65535, 'x');
	for (size_t i = 0; i < 245; ++i)
		list.Commands.push_back(command);
	gui::Router router;
	auto first = router.CapturePointerFrame(store, list);
	REQUIRE(first.Valid());
	const size_t frameBytes = first.RetainedBytes();
	REQUIRE(frameBytes < SpatialInputQueue::MAX_RETAINED_BYTES / 4);
	const size_t remaining = SpatialInputQueue::MAX_RETAINED_BYTES - 4 * frameBytes;
	REQUIRE(remaining > 3 * 64);
	const size_t each = (remaining > SpatialInputQueue::MAX_FRAME_METADATA_BYTES + 4 * 64)
							? (remaining - SpatialInputQueue::MAX_FRAME_METADATA_BYTES - 64 + 2) / 3
							: 1;
	REQUIRE(each <= SpatialInputQueue::MAX_FRAME_METADATA_BYTES);
	const size_t last = remaining - 3 * (each + 64) - 64;
	REQUIRE(remaining >= 3 * (each + 64) + 64);
	REQUIRE(last <= SpatialInputQueue::MAX_FRAME_METADATA_BYTES);
	std::vector<std::byte> metadata(each);
	SpatialInputQueue queue;
	REQUIRE(queue.Stage(1, std::move(first), 7, 12, {}, {}, metadata));
	for (uint64_t sequence = 2; sequence <= 3; ++sequence) {
		auto frame = router.CapturePointerFrame(store, list);
		REQUIRE(frame.Valid());
		REQUIRE(frame.RetainedBytes() == frameBytes);
		REQUIRE(queue.Stage(sequence, std::move(frame), 7, 12, {}, {}, metadata));
	}
	auto frame = router.CapturePointerFrame(store, list);
	REQUIRE(frame.Valid());
	REQUIRE(frame.RetainedBytes() == frameBytes);
	metadata.resize(last);
	// The variable payload alone exactly fills the budget; Frame objects cannot fit.
	CHECK_FALSE(queue.Stage(4, std::move(frame), 7, 12, {}, {}, metadata));
}
TEST_CASE(
	"failed queue cancellation reports opaque input only after retry succeeds", "[render][spatialqueue]"
) {
	DiscardCapture captured;
	SpatialInputQueue queue;
	FakeDepth depth;
	auto frame = MakeSpatialFrame();
	const auto store = frame.StoreIdentity();
	gui::Screen screen;
	screen.Width = 800;
	screen.Height = 600;
	REQUIRE(queue.Stage(1, std::move(frame), 7, 41, screen));
	depth.FrameStatus = SpatialDepthFrameStatus::Ready;
	depth.Frame.Key = {1, store, 7, 9};
	depth.Frame.SnapshotSequence = 1;
	depth.Frame.Width = 800;
	depth.Frame.Height = 600;
	FrameResult presented;
	presented.SpatialSnapshot = SpatialSnapshotStatus::Ready;
	presented.SpatialSnapshotSequence = 1;
	presented.SpatialDepth = depth.Frame.Key;
	REQUIRE(queue.Publish(presented, 7));
	gui::Pointer pointer;
	pointer.Position = {400, 300};
	REQUIRE(queue.EnqueuePointer(1, screen, pointer, 41));
	const std::array bytes{std::byte{12}};
	REQUIRE(queue.EnqueueOpaque(1, 41, bytes, false, 0, CaptureDiscard));
	REQUIRE(queue.SealBatch(41, store));
	REQUIRE(queue.Preflight(depth) == SpatialInputQueue::Status::Pending);
	depth.CanCancel = false;
	REQUIRE(queue.RequestCancel());
	CHECK(queue.Poll(depth).State == SpatialInputQueue::Status::Pending);
	CHECK(captured.Log.Count == 0);
	depth.CanCancel = true;
	const auto cancelled = queue.Poll(depth);
	REQUIRE(cancelled.State == SpatialInputQueue::Status::Cancelled);
	REQUIRE(captured.Log.Count == 1);
	CHECK(captured.Log.Entries[0].Reason == SpatialInputQueue::DiscardReason::Cancellation);
	CHECK(captured.Log.Entries[0].Value == std::byte{12});
	REQUIRE(queue.Acknowledge(cancelled.Event));
	REQUIRE(queue.Reset(depth));
	CHECK(captured.Log.Count == 1);
}
