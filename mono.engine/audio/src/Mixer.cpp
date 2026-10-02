#include <engine/audio/Mixer.hpp>
#include <engine/audio/Spatial.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cmath>
#include <span>

namespace engine::audio {

	namespace {
		// The part of a scratch buffer one segment actually uses.
		//
		// **Scratch is sized to the largest block the mixer will ever be asked
		// for, and a segment is usually far shorter than that.** A command with a
		// deadline inside the block splits it, and every piece re-renders every
		// node - so a node touching all 512 frames when the segment is eight of
		// them does sixty-four times the memory traffic the audio needs.
		//
		// That was measurable and large: `engine.audio.bench.mixing` reported 64
		// voices at 102 microseconds with no commands and 693 with sixty-four of
		// them, against a block deadline of 10.67 milliseconds. The work scaled
		// with the number of splits rather than with the number of frames, which
		// is the wrong axis entirely - the audio in a split block is exactly the
		// audio in an unsplit one.
		//
		// Nothing reads past `frames`: `MixSegment` copies that many and the
		// input-summing loop sums that many, and every segment re-silences from
		// zero before it writes. So the tail is not stale, it is simply never
		// looked at.
		std::span<float> Used(SampleBuffer &buffer, size_t frames, uint16_t channels) {
			const std::span<float> all = buffer.Data();
			return all.first(std::min(frames * static_cast<size_t>(channels), all.size()));
		}
	}

	AudioMixer::AudioMixer(AudioFormat format, size_t blockFrames)
		: Shape(format.IsValid() ? format : AudioFormat{}),
		  BlockFrames(blockFrames == 0 ? DEFAULT_BLOCK_FRAMES : blockFrames) {
		EnsureScratch();
	}

	void AudioMixer::EnsureScratch() {
		const std::span<const NodeId> order = Nodes.Order();

		// Rebuilt only when the node set changed. Comparing the id list is
		// cheaper than reallocating a buffer per node per block, and the node
		// set changes far less often than a block is rendered.
		if (ScratchFor.size() == order.size() &&
			std::equal(ScratchFor.begin(), ScratchFor.end(), order.begin())) {
			return;
		}

		ScratchFor.assign(order.begin(), order.end());
		Scratch.clear();
		Scratch.reserve(order.size());
		SlotOfNode.clear();
		SlotOfNode.reserve(order.size());
		for (size_t index = 0; index < order.size(); ++index) {
			Scratch.emplace_back(Shape, BlockFrames);
			SlotOfNode.emplace(order[index].Value, index);
		}
	}

	PlaybackStatus AudioMixer::Apply(const Command &command) {
		Node *target = Nodes.Find(command.Target);
		if (command.Kind != CommandKind::None && command.Kind != CommandKind::AddNode &&
			command.Kind != CommandKind::SetListener && target == nullptr) {
			return PlaybackStatus::MissingTarget;
		}
		if (target != nullptr && command.PlaybackGeneration != 0 && command.Kind != CommandKind::AddNode &&
			command.Kind != CommandKind::SetListener) {
			const bool stale = command.Kind == CommandKind::SetSound
								   ? command.PlaybackGeneration <= target->PlaybackGeneration
								   : command.PlaybackGeneration != target->PlaybackGeneration;
			if (stale) return PlaybackStatus::StaleGeneration;
		}
		switch (command.Kind) {
		case CommandKind::None:
			break;

		case CommandKind::AddNode:
			// The producer already named it, which is what makes creation
			// fire-and-forget. `AudioGraph::Add` mints its own ids, so the
			// command's id is adopted here instead.
			if (!Nodes.Adopt(command.Target, command.Node)) return PlaybackStatus::Refused;
			break;

		case CommandKind::RemoveNode:
			if (!Nodes.Remove(command.Target)) return PlaybackStatus::Refused;
			break;

		case CommandKind::Connect:
			if (!Nodes.Connect(command.Target, command.Second)) return PlaybackStatus::Refused;
			break;

		case CommandKind::Disconnect:
			if (!Nodes.Disconnect(command.Target, command.Second)) return PlaybackStatus::Refused;
			break;

		case CommandKind::SetSound:
			if (Node *node = target) {
				node->Sound = command.Sound;
				node->PlaybackGeneration = command.PlaybackGeneration;
				// Rewound with the sound. A cursor left where it was would
				// start the new sound part way through, which is never what
				// was meant.
				node->Cursor = 0.0;
			}
			break;

		case CommandKind::Play:
			if (Node *node = target) {
				node->Playing = true;
			}
			break;

		case CommandKind::Stop:
			if (Node *node = target) {
				node->Playing = false;
			}
			break;

		case CommandKind::Seek:
			if (!std::isfinite(command.CursorFrames) || command.CursorFrames < 0.0)
				return PlaybackStatus::InvalidCursor;
			if (target->Kind != NodeKind::Player || !target->Sound) return PlaybackStatus::MissingSource;
			if (command.CursorFrames > static_cast<double>(target->Sound->Frames()))
				return PlaybackStatus::OutOfRange;
			target->Cursor = command.CursorFrames;
			break;

		case CommandKind::Rewind:
			if (Node *node = target) {
				node->Cursor = 0.0;
			}
			break;

		case CommandKind::SetGain:
			if (Node *node = target) {
				// Negative gain is an inversion rather than silence, and
				// almost never meant. Clamped at zero; there is no ceiling,
				// because a deliberate boost is legitimate and the float
				// format carries it.
				node->Gain = std::max(0.0f, command.Value);
			}
			break;

		case CommandKind::SetPan:
			if (Node *node = target) {
				node->Pan = std::clamp(command.Value, -1.0f, 1.0f);
			}
			break;

		case CommandKind::SetMuted:
			if (Node *node = target) {
				node->Muted = command.Flag;
			}
			break;

		case CommandKind::SetLooping:
			if (Node *node = target) {
				node->Looping = command.Flag;
			}
			break;

		case CommandKind::SetPlacement:
			if (Node *node = target) {
				node->Placement = command.Placement;
			}
			break;

		case CommandKind::SetListener:
			Nodes.Listener() = command.Pose;
			break;
		}
		return PlaybackStatus::Applied;
	}

	void AudioMixer::PublishPlaybackEvent(const PlaybackEvent &event) {
		if (!PlaybackEventsEnabled) return;
		const size_t write = PlaybackWrite.load(std::memory_order_relaxed);
		const size_t read = PlaybackRead.load(std::memory_order_acquire);
		if (write - read == PLAYBACK_EVENT_CAPACITY) {
			MissedPlaybackEvents.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		PlaybackEvents[write & (PLAYBACK_EVENT_CAPACITY - 1)] = event;
		PlaybackWrite.store(write + 1, std::memory_order_release);
	}

	size_t AudioMixer::PollPlaybackEvents(std::span<PlaybackEvent> into) {
		const size_t write = PlaybackWrite.load(std::memory_order_acquire);
		const size_t read = PlaybackRead.load(std::memory_order_relaxed);
		const size_t count = std::min(into.size(), write - read);
		for (size_t index = 0; index < count; ++index)
			into[index] = PlaybackEvents[(read + index) & (PLAYBACK_EVENT_CAPACITY - 1)];
		PlaybackRead.store(read + count, std::memory_order_release);
		return count;
	}

	void AudioMixer::ApplyRecorded(const Command &command, size_t offset) {
		const Node *before = Nodes.Find(command.Target);
		const uint64_t priorGeneration = before == nullptr ? 0 : before->PlaybackGeneration;
		const PlaybackStatus status = Apply(command);
		const Node *after = Nodes.Find(command.Target);
		const AppliedAudioCommand applied{
			.Kind = command.Kind,
			.Target = command.Target,
			.Related = command.Second,
			.PlaybackGeneration = after == nullptr ? priorGeneration : after->PlaybackGeneration,
			.RequestedPlaybackGeneration = command.PlaybackGeneration,
			.RequestedSample = command.AtSample,
			.AppliedSample = Rendered + offset,
			.OffsetFrames = offset,
			.Status = status,
		};
		AppliedCommands[AppliedCommandCount++] = applied;
		if (!PlaybackEventsEnabled) return;
		PublishPlaybackEvent({
			.Applied = applied,
			.Status = applied.Status,
			.PlaybackGeneration = applied.PlaybackGeneration,
			.Present = after != nullptr,
			.Playing = after != nullptr && after->Playing,
			.CursorFrames = after == nullptr ? 0.0 : after->Cursor,
		});
	}

	void AudioMixer::FinishPlayer(NodeId id, Node &node, size_t offset) {
		const FinishedAudioSource finished{
			.Source = id,
			.PlaybackGeneration = node.PlaybackGeneration,
			.AtSample = Rendered + offset,
			.OffsetFrames = offset,
		};
		FinishedSources[FinishedSourceCount++] = finished;
		++FinishedThisBlock;
		if (!PlaybackEventsEnabled) return;
		PublishPlaybackEvent({
			.NaturalCompletion = true,
			.Finished = finished,
			.PlaybackGeneration = node.PlaybackGeneration,
			.Present = true,
			.Playing = false,
			.CursorFrames = node.Cursor,
		});
	}

	size_t AudioMixer::ApplyPending() {
		++CurrentObservationSerial;
		AppliedCommandCount = 0;
		FinishedSourceCount = 0;
		Taken.clear();
		Queue.Drain(Taken);
		for (const Command &command : Taken) {
			ApplyRecorded(command, 0);
		}
		EnsureScratch();
		return Taken.size();
	}

	void AudioMixer::RenderNode(size_t index, size_t frames, size_t blockOffset) {
		const NodeId id = ScratchFor[index];
		Node *node = Nodes.Find(id);
		SampleBuffer &out = Scratch[index];

		// Only this segment's frames, per `Used` above.
		const std::span<float> used = Used(out, frames, Shape.Channels);
		std::fill(used.begin(), used.end(), 0.0f);

		if (node == nullptr) {
			return;
		}

		if (node->Kind == NodeKind::Player) {
			// **A muted player still advances.** Unmuting resumes where the
			// sound would have been rather than where it was minutes ago, which
			// is what somebody expects of a mute button.
			if (!node->Playing || !node->Sound || node->Sound->Empty()) {
				return;
			}

			const SampleBuffer &source = *node->Sound;
			const size_t sourceFrames = source.Frames();
			// The rate ratio: how many source frames one output frame advances
			// by. A sound at the mixer's own rate steps exactly one.
			const double step =
				static_cast<double>(source.Format().SampleRate) / static_cast<double>(Shape.SampleRate);

			std::span<float> written = out.Data();
			for (size_t frame = 0; frame < frames; ++frame) {
				if (node->Cursor >= static_cast<double>(sourceFrames)) {
					if (!node->Looping) {
						node->Playing = false;
						FinishPlayer(id, *node, blockOffset + frame);
						break;
					}
					// Wrapped by subtraction rather than reset to zero, so a
					// loop keeps its fractional phase and does not drift a
					// fraction of a sample every time round.
					node->Cursor -= static_cast<double>(sourceFrames);
				}

				const auto at = static_cast<size_t>(node->Cursor);
				const std::span<const float> sample = source.Frame(std::min(at, sourceFrames - 1));

				if (!node->Muted) {
					for (uint16_t channel = 0; channel < Shape.Channels; ++channel) {
						const uint16_t sourceChannel =
							source.Format().Channels == 1
								? 0
								: std::min<uint16_t>(channel, source.Format().Channels - 1);
						written[frame * Shape.Channels + channel] = sample[sourceChannel] * node->Gain;
					}
				}
				node->Cursor += step;
			}
			// Reaching the source exactly at a segment boundary has no next
			// iteration to notice it. Stop and report it at that boundary so a
			// block ending with the final sample does not defer the event until
			// the following callback.
			if (node->Playing && !node->Looping && node->Cursor >= static_cast<double>(sourceFrames)) {
				node->Playing = false;
				FinishPlayer(id, *node, blockOffset + frames);
			}
			return;
		}

		// Every other kind sums what is wired into it and then does its own
		// thing to the sum. Gathering first is what makes a bus and a fader the
		// same code with one extra step.
		for (const NodeId source : Nodes.InputsOf(id)) {
			const auto found = SlotOfNode.find(source.Value);
			if (found == SlotOfNode.end()) {
				continue;
			}
			// The topological order guarantees the input was rendered before
			// this node, which is the whole reason the order exists.
			const std::span<const float> from = Scratch[found->second].Data();
			for (size_t sample = 0; sample < used.size(); ++sample) {
				used[sample] += from[sample];
			}
		}

		if (node->Muted) {
			std::fill(used.begin(), used.end(), 0.0f);
			return;
		}

		switch (node->Kind) {
		case NodeKind::Fader: {
			const StereoGain pan = PanGain(node->Pan);
			std::span<float> written = out.Data();
			for (size_t frame = 0; frame < frames; ++frame) {
				written[frame * Shape.Channels] *= node->Gain * pan.Left;
				if (Shape.Channels > 1) {
					written[frame * Shape.Channels + 1] *= node->Gain * pan.Right;
				}
			}
			break;
		}

		case NodeKind::Emitter: {
			const StereoGain placed = Place(Nodes.Listener(), node->Placement);
			std::span<float> written = out.Data();
			for (size_t frame = 0; frame < frames; ++frame) {
				written[frame * Shape.Channels] *= node->Gain * placed.Left;
				if (Shape.Channels > 1) {
					written[frame * Shape.Channels + 1] *= node->Gain * placed.Right;
				}
			}
			break;
		}

		case NodeKind::Bus:
		case NodeKind::Output: {
			// A plain sum, times the node's own gain. A bus with a gain of
			// one costs a multiply, which is cheaper than the branch that
			// would skip it.
			if (node->Gain != 1.0f) {
				for (float &sample : used) {
					sample *= node->Gain;
				}
			}
			break;
		}

		case NodeKind::Player:
			break;
		}
	}

	void AudioMixer::MixSegment(SampleBuffer &out, size_t offset, size_t frames) {
		if (frames == 0) {
			return;
		}

		for (size_t index = 0; index < ScratchFor.size(); ++index) {
			RenderNode(index, frames, offset);
		}

		// Copy the output's scratch into place.
		const auto found = SlotOfNode.find(Nodes.Output().Value);
		if (found == SlotOfNode.end()) {
			return;
		}
		const std::span<const float> from = Scratch[found->second].Data();
		std::span<float> written = out.Data();
		const size_t count = frames * Shape.Channels;
		for (size_t sample = 0; sample < count; ++sample) {
			written[offset * Shape.Channels + sample] = from[sample];
		}
	}

	MixReport AudioMixer::Render(SampleBuffer &out) {
		ENGINE_PROFILE("audio::AudioMixer::Render");

		MixReport report;
		++CurrentObservationSerial;
		report.BeginSample = Rendered;
		report.EndSample = Rendered;
		report.ObservationSerial = CurrentObservationSerial;
		AppliedCommandCount = 0;
		FinishedSourceCount = 0;
		if (out.Format() != Shape) {
			// Silence rather than a resample. A resample on this thread is the
			// wrong answer to a caller's configuration mistake, and silence is
			// at least diagnosable.
			out.Silence();
			return report;
		}

		const size_t frames = std::min(out.Frames(), BlockFrames);
		report.Frames = frames;
		FinishedThisBlock = 0;

		// Everything waiting, then sorted into where it lands inside this
		// block.
		Taken.clear();
		Queue.Drain(Taken);

		Schedule.clear();
		for (const Command &command : Taken) {
			size_t offset = 0;
			if (command.AtSample > Rendered) {
				const uint64_t ahead = command.AtSample - Rendered;
				// Past the end of this block: it still has to be applied
				// eventually, and the simplest correct answer is to hold it to
				// the last sample of this one rather than carry it. A command
				// scheduled further ahead than one block is a caller asking for
				// something this queue does not promise.
				offset = ahead >= frames ? frames : static_cast<size_t>(ahead);
			}
			// A deadline in the past lands at offset zero - applied at the
			// start of the next block rather than dropped. A tick that ran late
			// still meant its command to happen.
			Schedule.push_back(Due{.What = command, .Offset = offset});
		}

		// Stable, so two commands due on one sample are applied in the order
		// they were posted. "Set the gain then play" and "play then set the
		// gain" are different, and the caller's order is the one that means
		// something.
		std::stable_sort(Schedule.begin(), Schedule.end(), [](const Due &left, const Due &right) {
			return left.Offset < right.Offset;
		});

		// Walk the block, stopping at each distinct deadline.
		size_t at = 0;
		size_t next = 0;
		while (at < frames) {
			// Apply everything due exactly here.
			while (next < Schedule.size() && Schedule[next].Offset <= at) {
				const Due &due = Schedule[next];
				ApplyRecorded(due.What, at);
				++report.Applied;
				++next;
			}
			EnsureScratch();

			const size_t until = next < Schedule.size() ? std::min(Schedule[next].Offset, frames) : frames;
			const size_t length = until - at;
			if (length > 0) {
				MixSegment(out, at, length);
				++report.Segments;
			}
			at = until;
		}

		// Anything left is due at or past the end of the block. Applied now so
		// it takes effect at the start of the next one rather than being
		// dropped.
		while (next < Schedule.size()) {
			const Due &due = Schedule[next];
			ApplyRecorded(due.What, frames);
			++report.Applied;
			++next;
		}
		EnsureScratch();

		// `Segments` counts the pieces actually mixed; a block with no commands
		// in it is one.
		report.Segments = std::max<size_t>(1, report.Segments);
		report.Finished = FinishedThisBlock;

		// **Clipped exactly once, here, at the end.** The whole graph runs in
		// float precisely so it can exceed ±1.0 without harm; a device takes
		// samples in range, so this is where the range starts to matter.
		for (float &sample : out.Data()) {
			// Keep the meter's `SampleBuffer::Peak` semantics: NaN does not
			// replace an earlier finite peak, while infinity remains visible.
			report.Peak = std::max(report.Peak, std::abs(sample));
			if (sample > 1.0f || sample < -1.0f) {
				sample = std::clamp(sample, -1.0f, 1.0f);
				report.Clipped = true;
			}
		}

		Rendered += frames;
		report.EndSample = Rendered;
		return report;
	}
}
