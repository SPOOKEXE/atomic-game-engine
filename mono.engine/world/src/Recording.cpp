#include <engine/core/Log.hpp>
#include <engine/world/Recording.hpp>

namespace engine::world {

	namespace {
		// Recognises a recording before anything else is read.
		constexpr uint64_t RECORDING_MAGIC = 0x4443'4552'4F4E'4F4Dull;
	}

	// --- Recorder ----------------------------------------------------------

	bool Recorder::Begin(const Universe &universe) {
		Clear();

		core::ByteWriter writer;
		if (!universe.Save(writer)) {
			ENGINE_ERROR("recorder: the universe cannot be snapshotted, so it cannot be recorded.");
			return false;
		}

		Initial.assign(writer.Bytes().begin(), writer.Bytes().end());
		Started = true;
		return true;
	}

	void Recorder::Capture(const Universe &universe, float frameSeconds) {
		if (!Started) {
			return;
		}

		RecordedBarrier barrier;
		barrier.FrameSeconds = frameSeconds;

		const std::span<const Envelope> traffic = universe.LastTraffic();
		barrier.Traffic.assign(traffic.begin(), traffic.end());

		Timeline.push_back(std::move(barrier));
	}

	bool Recorder::Write(core::ByteWriter &writer) const {
		if (!Started) {
			return false;
		}

		writer.WriteUInt64(RECORDING_MAGIC);
		writer.WriteUInt32(VERSION);

		writer.WriteUInt32(static_cast<uint32_t>(Initial.size()));
		writer.WriteRaw(Initial.data(), Initial.size());

		writer.WriteUInt32(static_cast<uint32_t>(Timeline.size()));
		for (const RecordedBarrier &barrier : Timeline) {
			writer.WriteFloat(barrier.FrameSeconds);
			writer.WriteUInt32(static_cast<uint32_t>(barrier.Traffic.size()));
			for (const Envelope &envelope : barrier.Traffic) {
				WriteEnvelope(writer, envelope);
			}
		}

		return true;
	}

	void Recorder::Clear() {
		Started = false;
		Initial.clear();
		Timeline.clear();
	}

	// --- Replayer ----------------------------------------------------------

	bool Replayer::Load(core::ByteReader &reader) {
		Initial.clear();
		Timeline.clear();
		Position = 0;

		if (reader.ReadUInt64() != RECORDING_MAGIC) {
			ENGINE_ERROR("replayer: not a recording.");
			return false;
		}

		const uint32_t version = reader.ReadUInt32();
		if (version != Recorder::VERSION) {
			ENGINE_ERROR("replayer: recording version {}, this build reads {}.", version, Recorder::VERSION);
			return false;
		}

		const uint32_t initial = reader.ReadUInt32();
		Initial.resize(reader.Failed() ? 0 : initial);
		if (!Initial.empty()) {
			reader.ReadRaw(Initial.data(), Initial.size());
		}

		const uint32_t barriers = reader.ReadUInt32();
		for (uint32_t index = 0; index < barriers && !reader.Failed(); index++) {
			RecordedBarrier barrier;
			barrier.FrameSeconds = reader.ReadFloat();

			const uint32_t traffic = reader.ReadUInt32();
			for (uint32_t at = 0; at < traffic && !reader.Failed(); at++) {
				barrier.Traffic.push_back(ReadEnvelope(reader));
			}

			Timeline.push_back(std::move(barrier));
		}

		if (reader.Failed()) {
			// Left empty rather than partly loaded, for the same reason a store
			// is: half a recording replays into a world nobody can reason about.
			Initial.clear();
			Timeline.clear();
			return false;
		}

		return true;
	}

	bool
	Replayer::Restore(Universe &universe, const std::function<void(Universe &, WorldId)> &configure) const {
		core::ByteReader reader(Initial);
		if (!universe.Load(reader)) {
			return false;
		}

		if (configure) {
			for (const WorldId id : universe.Worlds()) {
				configure(universe, id);
			}
		}
		return true;
	}

	bool Replayer::Step(Universe &universe) {
		if (Position >= Timeline.size()) {
			return false;
		}

		const RecordedBarrier &barrier = Timeline[Position++];
		LastFrame = barrier.FrameSeconds;

		// The traffic goes in before the tick, so the barrier applies what was
		// recorded rather than what this run's worlds happen to have queued.
		universe.InjectTraffic(barrier.Traffic);
		universe.Tick(barrier.FrameSeconds);
		return true;
	}

	size_t Replayer::Run(Universe &universe) {
		size_t played = 0;
		while (Step(universe)) {
			played++;
		}
		return played;
	}
}
