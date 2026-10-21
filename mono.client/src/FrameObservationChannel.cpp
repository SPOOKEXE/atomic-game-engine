#include "FrameObservationChannel.hpp"

#include "FrameObservation.hpp"

#include <engine/core/Bytes.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace client {
	namespace {
		using engine::core::ByteReader;
		using engine::core::ByteWriter;
		using engine::parallel::ChannelStatus;
		constexpr std::string_view REQUEST_SCHEMA = "client.native-frame-observation/request/1";
		constexpr std::string_view SUMMARY_SCHEMA = "client.native-frame-observation/summary/1";
		constexpr std::string_view ACK_SCHEMA = "client.native-frame-observation/ack/1";
		constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(2);
		constexpr size_t MAXIMUM_TEXT = 4096;
		constexpr size_t MAXIMUM_OWNED_BYTES = 8u * 1024u * 1024u;

		bool ValidRequest(const FrameObservationChannelRequest &request) {
			const auto field = [](const std::string &name) {
				return !name.empty() && name.size() <= 64 && name.find('\0') == std::string::npos;
			};
			return request.Run != 0 && request.MaxOwnedBytes > 0 &&
				   request.MaxOwnedBytes <= MAXIMUM_OWNED_BYTES && field(request.RetainedPresenceField) &&
				   field(request.RetainedIntegerField);
		}

		std::string ReadText(ByteReader &reader, size_t maximum = MAXIMUM_TEXT) {
			const auto text = reader.ReadString();
			if (text.size() > maximum || text.find('\0') != std::string_view::npos) {
				reader.Fail();
				return {};
			}
			return std::string(text);
		}

		void WriteText(ByteWriter &writer, std::string_view text, size_t maximum = MAXIMUM_TEXT) {
			if (text.size() > maximum || text.find('\0') != std::string_view::npos)
				throw std::length_error("frame observation text exceeds its bound");
			writer.WriteString(text);
		}

		bool MatchesInteger(const nlohmann::json &value, uint64_t expected) {
			if (value.is_number_unsigned()) return value.get<uint64_t>() == expected;
			return value.is_number_integer() && value.get<int64_t>() >= 0 &&
				   static_cast<uint64_t>(value.get<int64_t>()) == expected;
		}

		bool Eligible(const FrameObservation &frame, const FrameObservationChannelRequest &request) {
			const auto integer = frame.Metadata.find(request.RetainedIntegerField);
			return frame.Metadata.contains(request.RetainedPresenceField) &&
				   integer != frame.Metadata.end() && MatchesInteger(*integer, request.RetainedIntegerValue);
		}

		// Match the native sampler's conservative charge for JSON nodes and capacity.
		// Decoder construction is a bounded transient; retained records must fit the request.
		size_t JsonOwnedBytes(const nlohmann::json &value) {
			size_t bytes = 128;
			if (value.is_string()) return bytes + value.get_ref<const std::string &>().capacity();
			if (value.is_array())
				bytes += value.get_ref<const nlohmann::json::array_t &>().capacity() * sizeof(nlohmann::json);
			if (value.is_structured())
				for (auto entry = value.begin(); entry != value.end(); ++entry) {
					if (value.is_object()) bytes += entry.key().capacity();
					bytes += JsonOwnedBytes(entry.value());
				}
			return bytes;
		}

		bool ValidFrame(const FrameObservation &frame) {
			const auto &image = frame.Image;
			const uint64_t pixelBytes = static_cast<uint64_t>(image.Width) * image.Height * 4;
			if (frame.Generation == 0 || image.Token == 0 || image.Frame == 0 || image.Width == 0 ||
				image.Height == 0 || pixelBytes > FRAME_OBSERVATION_CHANNEL_MAXIMUM_PIXELS ||
				image.RowStride != static_cast<size_t>(image.Width) * 4 ||
				image.Pixels.size() != pixelBytes ||
				image.Status != engine::render::SceneCaptureStatus::Ready ||
				(image.Format != engine::render::ResourceImageFormat::RGBA8_UNorm &&
				 image.Format != engine::render::ResourceImageFormat::RGBA8_SRGB) ||
				image.WorldName.empty() || image.Pipeline.Text().empty() || image.PipelineRevision == 0 ||
				frame.Pipeline != image.Pipeline.Text() || !frame.Metadata.is_object())
				return false;
			const auto frameNumber = frame.Metadata.find("frame");
			const auto generation = frame.Metadata.find("observation_generation");
			const auto world = frame.Metadata.find("view_world");
			if (frameNumber == frame.Metadata.end() || !MatchesInteger(*frameNumber, frame.ClientFrame) ||
				generation == frame.Metadata.end() || !MatchesInteger(*generation, frame.Generation) ||
				world == frame.Metadata.end() || !world->is_string() || *world != image.WorldName)
				return false;
			const auto snapshot = frame.Metadata.find("snapshot_id");
			const auto slot = frame.Metadata.find("view_slot");
			const auto pipeline = frame.Metadata.find("pipeline");
			if (snapshot == frame.Metadata.end() || !snapshot->is_string() || *snapshot != image.SnapshotId ||
				slot == frame.Metadata.end() || !MatchesInteger(*slot, image.ViewSlot) ||
				pipeline == frame.Metadata.end() || !pipeline->is_object())
				return false;
			const auto name = pipeline->find("name");
			const auto revision = pipeline->find("revision");
			if (name == pipeline->end() || !name->is_string() || *name != frame.Pipeline ||
				revision == pipeline->end() || !MatchesInteger(*revision, image.PipelineRevision))
				return false;
			const auto &pose = image.CameraFrame;
			const std::array values{
				pose.Position.X,
				pose.Position.Y,
				pose.Position.Z,
				pose.QuaternionX,
				pose.QuaternionY,
				pose.QuaternionZ,
				pose.QuaternionW,
				image.Camera.FieldOfViewRadians,
				image.Camera.NearPlane,
				image.Camera.FarPlane
			};
			for (const auto value : values)
				if (!std::isfinite(value)) return false;
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					if (!std::isfinite(image.Projection[column][row])) return false;
			return image.Camera.NearPlane > 0 && image.Camera.FarPlane > image.Camera.NearPlane &&
				   image.Camera.FieldOfViewRadians > 0;
		}

		void WriteFrame(ByteWriter &writer, const FrameObservation &frame) {
			if (!ValidFrame(frame)) throw std::invalid_argument("incomplete native frame observation");
			const auto &image = frame.Image;
			writer.WriteUInt64(frame.Generation);
			writer.WriteUInt64(frame.ClientFrame);
			writer.WriteUInt64(image.Token);
			writer.WriteUInt64(image.Frame);
			// WorldName is the cross-process identity. World is only a producer-local diagnostic value.
			writer.WriteUInt64(image.World);
			WriteText(writer, image.WorldName);
			WriteText(writer, image.SnapshotId);
			WriteText(writer, frame.Pipeline);
			writer.WriteUInt64(image.PipelineRevision);
			writer.WriteUInt64(image.ViewSlot);
			WriteText(
				writer,
				image.Format == engine::render::ResourceImageFormat::RGBA8_SRGB ? "rgba8_srgb" : "rgba8_unorm"
			);
			writer.WriteUInt32(image.Width);
			writer.WriteUInt32(image.Height);
			writer.WriteUInt64(image.RowStride);
			const auto &pose = image.CameraFrame;
			for (const auto value : std::array{
					 pose.Position.X,
					 pose.Position.Y,
					 pose.Position.Z,
					 pose.QuaternionX,
					 pose.QuaternionY,
					 pose.QuaternionZ,
					 pose.QuaternionW,
					 image.Camera.FieldOfViewRadians,
					 image.Camera.NearPlane,
					 image.Camera.FarPlane
				 })
				writer.WriteFloat(value);
			for (const auto value : std::array{
					 image.Camera.MaxImageWidth,
					 image.Camera.MaxImageHeight,
					 image.Camera.ImageWidth,
					 image.Camera.ImageHeight,
					 image.Camera.RenderFeatures.Enable,
					 image.Camera.RenderFeatures.Disable
				 })
				writer.WriteUInt32(value);
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					writer.WriteFloat(image.Projection[column][row]);
			WriteText(writer, frame.Metadata.dump(), FRAME_OBSERVATION_CHANNEL_MAXIMUM_METADATA);
			writer.WriteUInt32(static_cast<uint32_t>(image.Pixels.size()));
			writer.WriteRaw(image.Pixels.data(), image.Pixels.size());
		}

		std::shared_ptr<const FrameObservation> ReadFrame(ByteReader &reader, size_t maximumOwnedBytes) {
			auto frame = std::make_shared<FrameObservation>();
			auto &image = frame->Image;
			frame->Generation = reader.ReadUInt64();
			frame->ClientFrame = reader.ReadUInt64();
			image.Token = reader.ReadUInt64();
			image.Frame = reader.ReadUInt64();
			image.World = reader.ReadUInt64();
			image.WorldName = ReadText(reader);
			image.SnapshotId = ReadText(reader);
			frame->Pipeline = ReadText(reader);
			image.Pipeline = engine::core::Name(frame->Pipeline);
			image.PipelineRevision = reader.ReadUInt64();
			const uint64_t slot = reader.ReadUInt64();
			if (slot > std::numeric_limits<size_t>::max()) reader.Fail();
			image.ViewSlot = static_cast<size_t>(slot);
			const auto format = ReadText(reader, 32);
			if (format == "rgba8_srgb")
				image.Format = engine::render::ResourceImageFormat::RGBA8_SRGB;
			else if (format == "rgba8_unorm")
				image.Format = engine::render::ResourceImageFormat::RGBA8_UNorm;
			else
				reader.Fail();
			image.Width = reader.ReadUInt32();
			image.Height = reader.ReadUInt32();
			const uint64_t stride = reader.ReadUInt64();
			if (stride > std::numeric_limits<size_t>::max()) reader.Fail();
			image.RowStride = static_cast<size_t>(stride);
			auto &pose = image.CameraFrame;
			for (auto *value : std::array{
					 &pose.Position.X,
					 &pose.Position.Y,
					 &pose.Position.Z,
					 &pose.QuaternionX,
					 &pose.QuaternionY,
					 &pose.QuaternionZ,
					 &pose.QuaternionW,
					 &image.Camera.FieldOfViewRadians,
					 &image.Camera.NearPlane,
					 &image.Camera.FarPlane
				 })
				*value = reader.ReadFloat();
			for (auto *value : std::array{
					 &image.Camera.MaxImageWidth,
					 &image.Camera.MaxImageHeight,
					 &image.Camera.ImageWidth,
					 &image.Camera.ImageHeight,
					 &image.Camera.RenderFeatures.Enable,
					 &image.Camera.RenderFeatures.Disable
				 })
				*value = reader.ReadUInt32();
			for (size_t column = 0; column < 4; ++column)
				for (size_t row = 0; row < 4; ++row)
					image.Projection[column][row] = reader.ReadFloat();
			const auto metadata = ReadText(reader, FRAME_OBSERVATION_CHANNEL_MAXIMUM_METADATA);
			const auto pixels = reader.ReadUInt32();
			if (reader.Failed() || pixels == 0 || pixels > FRAME_OBSERVATION_CHANNEL_MAXIMUM_PIXELS ||
				pixels != static_cast<uint64_t>(image.Width) * image.Height * 4 ||
				image.RowStride != static_cast<uint64_t>(image.Width) * 4) {
				reader.Fail();
				return {};
			}
			const auto pixelBytes = reader.ReadRawView(pixels);
			if (reader.Failed()) return {};
			frame->Metadata = nlohmann::json::parse(metadata, [](int depth, auto, auto &) {
				if (depth > 32) throw std::invalid_argument("frame metadata nesting exceeds its bound");
				return true;
			});
			frame->OwnedBytes = sizeof(FrameObservation) + 64 + JsonOwnedBytes(frame->Metadata) +
								image.WorldName.capacity() + image.SnapshotId.capacity() +
								frame->Pipeline.capacity() * 2 + pixels;
			if (frame->OwnedBytes > maximumOwnedBytes) {
				reader.Fail();
				return {};
			}
			image.Pixels.resize(pixels);
			std::memcpy(image.Pixels.data(), pixelBytes.data(), pixels);
			image.Status = engine::render::SceneCaptureStatus::Ready;
			if (!ValidFrame(*frame)) {
				reader.Fail();
				return {};
			}
			return frame;
		}

		bool ReceiveHandshake(
			engine::parallel::Channel &channel, std::vector<std::byte> &bytes, std::string &failure
		) {
			const auto deadline = std::chrono::steady_clock::now() + HANDSHAKE_TIMEOUT;
			while (std::chrono::steady_clock::now() < deadline) {
				const auto status = channel.Receive(bytes);
				if (status == ChannelStatus::Ok) return true;
				if (status != ChannelStatus::Empty) {
					failure = "frame observation channel closed during handshake";
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			failure = "frame observation channel handshake timed out";
			return false;
		}
	}

	engine::parallel::ChannelSettings FrameObservationChannelSettings() {
		return {
			.MaximumFrame = FRAME_OBSERVATION_CHANNEL_MAXIMUM_WIRE,
			.Capacity = 2 * FRAME_OBSERVATION_CHANNEL_MAXIMUM_WIRE
		};
	}

	std::vector<std::byte>
	EncodeFrameObservationChannelRequest(const FrameObservationChannelRequest &request) {
		if (!ValidRequest(request)) throw std::invalid_argument("invalid frame observation request");
		ByteWriter writer(256, 4096);
		writer.WriteString(REQUEST_SCHEMA);
		writer.WriteUInt64(request.Run);
		writer.WriteUInt64(request.MaxOwnedBytes);
		writer.WriteString(request.RetainedPresenceField);
		writer.WriteString(request.RetainedIntegerField);
		writer.WriteUInt64(request.RetainedIntegerValue);
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	bool DecodeFrameObservationChannelRequest(
		std::span<const std::byte> bytes,
		uint64_t run,
		FrameObservationChannelRequest &out,
		std::string &failure
	) {
		if (bytes.size() > 4096) {
			failure = "oversized frame observation request";
			return false;
		}
		ByteReader reader(bytes);
		FrameObservationChannelRequest request;
		const auto schema = ReadText(reader, 64);
		request.Run = reader.ReadUInt64();
		const auto ownedBytes = reader.ReadUInt64();
		if (ownedBytes > MAXIMUM_OWNED_BYTES) reader.Fail();
		request.MaxOwnedBytes = static_cast<size_t>(ownedBytes);
		request.RetainedPresenceField = ReadText(reader, 64);
		request.RetainedIntegerField = ReadText(reader, 64);
		request.RetainedIntegerValue = reader.ReadUInt64();
		if (!reader.AtEnd() || schema != REQUEST_SCHEMA || request.Run != run || !ValidRequest(request)) {
			failure = "invalid or stale frame observation request";
			return false;
		}
		out = std::move(request);
		failure.clear();
		return true;
	}

	bool RetainFrameObservationChannel(
		FrameObservationChannelSummary &summary,
		const FrameObservationChannelRequest &request,
		std::shared_ptr<const FrameObservation> frame,
		std::string &failure
	) {
		if (!ValidRequest(request) || !frame || frame->OwnedBytes == 0 || !ValidFrame(*frame) ||
			(summary.Generation && summary.Generation != frame->Generation) ||
			(summary.Latest && (frame->ClientFrame <= summary.Latest->ClientFrame ||
								frame->Image.Frame <= summary.Latest->Image.Frame ||
								frame->Image.Token == summary.Latest->Image.Token))) {
			failure = "incomplete, stale or unordered native frame observation";
			return false;
		}
		const bool eligible = Eligible(*frame, request);
		const size_t retainedBytes =
			!eligible && summary.Retained && summary.Retained != frame ? summary.Retained->OwnedBytes : 0;
		if (frame->OwnedBytes > request.MaxOwnedBytes ||
			retainedBytes > request.MaxOwnedBytes - frame->OwnedBytes) {
			failure = "retained native frame observations exceed their byte budget";
			return false;
		}
		summary.Run = request.Run;
		summary.Generation = frame->Generation;
		++summary.CapturedFrames;
		summary.Latest = frame;
		if (eligible) summary.Retained = std::move(frame);
		failure.clear();
		return true;
	}

	std::vector<std::byte>
	EncodeFrameObservationChannelSummary(const FrameObservationChannelSummary &summary) {
		if (summary.Run == 0 ||
			(summary.Complete && (!summary.Generation || !summary.CapturedFrames || !summary.Latest ||
								  !summary.Failure.empty() || summary.RunResult != 0)))
			throw std::invalid_argument("incomplete frame observation summary");
		ByteWriter writer(4096, FRAME_OBSERVATION_CHANNEL_MAXIMUM_WIRE);
		writer.WriteString(SUMMARY_SCHEMA);
		writer.WriteUInt64(summary.Run);
		writer.WriteUInt64(summary.Generation);
		writer.WriteUInt64(summary.CapturedFrames);
		writer.WriteInt32(summary.RunResult);
		writer.WriteUInt8(summary.Complete ? 1 : 0);
		WriteText(writer, summary.Failure);
		writer.WriteUInt8(summary.Latest ? 1 : 0);
		if (summary.Latest) WriteFrame(writer, *summary.Latest);
		writer.WriteUInt8(summary.Retained ? 1 : 0);
		if (summary.Retained) WriteFrame(writer, *summary.Retained);
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	bool DecodeFrameObservationChannelSummary(
		std::span<const std::byte> bytes,
		const FrameObservationChannelRequest &request,
		FrameObservationChannelSummary &out,
		std::string &failure
	) {
		if (!ValidRequest(request) || bytes.size() > FRAME_OBSERVATION_CHANNEL_MAXIMUM_WIRE) {
			failure = "oversized frame observation summary or invalid request";
			return false;
		}
		try {
			ByteReader reader(bytes);
			const auto schema = ReadText(reader, 64);
			FrameObservationChannelSummary summary;
			summary.Run = reader.ReadUInt64();
			summary.Generation = reader.ReadUInt64();
			summary.CapturedFrames = reader.ReadUInt64();
			summary.RunResult = reader.ReadInt32();
			const auto complete = reader.ReadUInt8();
			summary.Complete = complete == 1;
			summary.Failure = ReadText(reader);
			const auto hasLatest = reader.ReadUInt8();
			if (hasLatest == 1) summary.Latest = ReadFrame(reader, request.MaxOwnedBytes);
			const auto hasRetained = reader.ReadUInt8();
			if (hasRetained == 1)
				summary.Retained = ReadFrame(
					reader, request.MaxOwnedBytes - (summary.Latest ? summary.Latest->OwnedBytes : 0)
				);
			if (!reader.AtEnd() || schema != SUMMARY_SCHEMA || summary.Run != request.Run || complete > 1 ||
				hasLatest > 1 || hasRetained > 1 || (hasLatest && !summary.Latest) ||
				(hasRetained && !summary.Retained) ||
				(summary.Latest && summary.Latest->Generation != summary.Generation) ||
				(summary.Retained && (summary.Retained->Generation != summary.Generation ||
									  !Eligible(*summary.Retained, request) || !summary.Latest ||
									  summary.Retained->ClientFrame > summary.Latest->ClientFrame ||
									  summary.Retained->Image.Frame > summary.Latest->Image.Frame)) ||
				(summary.Complete && (!summary.Latest || !summary.Generation || !summary.CapturedFrames ||
									  !summary.Failure.empty() || summary.RunResult != 0)) ||
				(!summary.Complete && summary.Failure.empty())) {
				failure = "incomplete, malformed or stale frame observation summary";
				return false;
			}
			out = std::move(summary);
			failure.clear();
			return true;
		} catch (const std::exception &error) {
			failure = std::string("invalid frame observation summary: ") + error.what();
			return false;
		}
	}

	std::vector<std::byte>
	EncodeFrameObservationChannelAcknowledgement(uint64_t run, uint64_t generation, bool accepted) {
		ByteWriter writer;
		writer.WriteString(ACK_SCHEMA);
		writer.WriteUInt64(run);
		writer.WriteUInt64(generation);
		writer.WriteUInt8(accepted ? 1 : 0);
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	bool
	FrameObservationChannelAcknowledged(std::span<const std::byte> bytes, uint64_t run, uint64_t generation) {
		if (bytes.size() > 4096) return false;
		ByteReader reader(bytes);
		const auto schema = ReadText(reader, 64);
		const auto receivedRun = reader.ReadUInt64();
		const auto receivedGeneration = reader.ReadUInt64();
		const auto accepted = reader.ReadUInt8();
		return reader.AtEnd() && schema == ACK_SCHEMA && run != 0 && generation != 0 && receivedRun == run &&
			   receivedGeneration == generation && accepted == 1;
	}

	struct FrameObservationChannel::Impl {
		std::unique_ptr<engine::parallel::Channel> Link;
		FrameObservationChannelRequest Request;
		FrameObservationChannelSummary Summary;
		FrameObservationLease Lease;
		std::string Error;
		bool Finished = false;
	};

	FrameObservationChannel::FrameObservationChannel(
		Client &client, std::unique_ptr<engine::parallel::Channel> channel, uint64_t run
	)
		: State(std::make_unique<Impl>()) {
		State->Link = std::move(channel);
		std::vector<std::byte> bytes;
		if (!State->Link) {
			State->Error = "native frame observation requires an inherited channel";
			return;
		}
		if (!ReceiveHandshake(*State->Link, bytes, State->Error) ||
			!DecodeFrameObservationChannelRequest(bytes, run, State->Request, State->Error))
			return;
		FrameObservationOptions options;
		options.MaxPendingFrames = 3;
		options.MaxOwnedBytes = State->Request.MaxOwnedBytes;
		State->Lease = SubscribeClientFrames(client, options, [this](auto frame) {
			if (!State->Error.empty()) return;
			if (!RetainFrameObservationChannel(
					State->Summary, State->Request, std::move(frame), State->Error
				))
				throw std::runtime_error(State->Error);
		});
		if (!State->Lease) State->Error = State->Lease.Failure();
	}

	FrameObservationChannel::~FrameObservationChannel() = default;
	bool FrameObservationChannel::Ready() const {
		return State->Error.empty() && bool(State->Lease);
	}
	std::string FrameObservationChannel::Failure() const {
		return State->Error;
	}

	bool FrameObservationChannel::Finish(int runResult) {
		if (State->Finished) return false;
		State->Finished = true;
		auto &summary = State->Summary;
		summary.Run = State->Request.Run;
		summary.Generation = State->Lease.Generation();
		summary.RunResult = runResult;
		const bool observed = State->Lease.Finish();
		if (!observed && State->Error.empty()) State->Error = State->Lease.Failure();
		if ((summary.CapturedFrames != State->Lease.CapturedFrames() || !summary.Latest) &&
			State->Error.empty())
			State->Error = "native frame observation completion count is incomplete";
		if (runResult != 0 && State->Error.empty()) State->Error = "native client Run returned failure";
		summary.Complete = observed && State->Error.empty();
		summary.Failure = State->Error;
		std::vector<std::byte> bytes;
		try {
			bytes = EncodeFrameObservationChannelSummary(summary);
		} catch (const std::exception &error) {
			State->Error = error.what();
		}
		summary.Latest.reset();
		summary.Retained.reset();
		if (State->Lease.OwnedBytes() != 0)
			State->Error = "native frame observations retained bytes after release";
		if (!State->Error.empty()) {
			summary.Complete = false;
			summary.Failure = State->Error.substr(0, MAXIMUM_TEXT);
			bytes = EncodeFrameObservationChannelSummary(summary);
		}
		if (!State->Link || State->Link->Send(bytes) != ChannelStatus::Ok) {
			State->Error = "could not send completed native frame observations";
			return false;
		}
		std::vector<std::byte> acknowledgement;
		const bool acknowledged =
			ReceiveHandshake(*State->Link, acknowledgement, State->Error) &&
			FrameObservationChannelAcknowledged(acknowledgement, summary.Run, summary.Generation);
		if (!acknowledged && State->Error.empty())
			State->Error = "native frame acknowledgement was refused or stale";
		State->Link->Close();
		return acknowledged && summary.Complete;
	}
}
