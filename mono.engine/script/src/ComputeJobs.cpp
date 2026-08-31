#include <engine/core/Bytes.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/script/ComputeJobs.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace engine::script {
	enum class WorkState : uint8_t {
		Pending,
		Complete,
		Failed,
		Cancelled,
	};

	namespace {
		constexpr uint32_t PROTOCOL_MAGIC = 0x4E4F4953u;
		constexpr uint16_t PROTOCOL_VERSION = 1;
		constexpr std::string_view WORKER_ARGUMENT = "--engine-compute-worker";

		struct WorkerConfiguration {
			std::filesystem::path Program;
			std::vector<std::string> Arguments;
		};

		WorkerConfiguration &Worker() {
			static WorkerConfiguration worker;
			return worker;
		}

		std::mutex &WorkerProgramGuard() {
			static std::mutex guard;
			return guard;
		}

		constexpr std::array<uint8_t, 257> PERLIN_HASH{
			151, 160, 137, 91,	90,	 15,  131, 13,	201, 95,  96,  53,	194, 233, 7,   225, 140, 36,  103,
			30,	 69,  142, 8,	99,	 37,  240, 21,	10,	 23,  190, 6,	148, 247, 120, 234, 75,	 0,	  26,
			197, 62,  94,  252, 219, 203, 117, 35,	11,	 32,  57,  177, 33,	 88,  237, 149, 56,	 87,  174,
			20,	 125, 136, 171, 168, 68,  175, 74,	165, 71,  134, 139, 48,	 27,  166, 77,	146, 158, 231,
			83,	 111, 229, 122, 60,	 211, 133, 230, 220, 105, 92,  41,	55,	 46,  245, 40,	244, 102, 143,
			54,	 65,  25,  63,	161, 1,	  216, 80,	73,	 209, 76,  132, 187, 208, 89,  18,	169, 200, 196,
			135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,	 64,  52,  217, 226, 250, 124,
			123, 5,	  202, 38,	147, 118, 126, 255, 82,	 85,  212, 207, 206, 59,  227, 47,	16,	 58,  17,
			182, 189, 28,  42,	223, 183, 170, 213, 119, 248, 152, 2,	44,	 154, 163, 70,	221, 153, 101,
			155, 167, 43,  172, 9,	 129, 22,  39,	253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185,
			112, 104, 218, 246, 97,	 228, 251, 34,	242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,
			51,	 145, 235, 249, 14,	 239, 107, 49,	192, 214, 31,  181, 199, 106, 157, 184, 84,	 204, 176,
			115, 121, 50,  45,	127, 4,	  150, 254, 138, 236, 205, 93,	222, 114, 67,  29,	24,	 72,  243,
			141, 128, 195, 78,	66,	 215, 61,  156, 180, 151,
		};

		constexpr std::array<std::array<float, 3>, 16> PERLIN_GRAD{{
			{{1, 1, 0}},
			{{-1, 1, 0}},
			{{1, -1, 0}},
			{{-1, -1, 0}},
			{{1, 0, 1}},
			{{-1, 0, 1}},
			{{1, 0, -1}},
			{{-1, 0, -1}},
			{{0, 1, 1}},
			{{0, -1, 1}},
			{{0, 1, -1}},
			{{0, -1, -1}},
			{{1, 1, 0}},
			{{0, -1, 1}},
			{{-1, 1, 0}},
			{{0, -1, -1}},
		}};

		float Fade(float value) {
			return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
		}

		float Lerp(float alpha, float first, float second) {
			return first + alpha * (second - first);
		}

		float Gradient(int hash, float x, float y, float z) {
			const auto &gradient = PERLIN_GRAD[static_cast<size_t>(hash & 15)];
			return gradient[0] * x + gradient[1] * y + gradient[2] * z;
		}

		float Noise(double sourceX, double sourceY, double sourceZ) {
			const float x = static_cast<float>(std::fmod(sourceX, 256.0));
			const float y = static_cast<float>(std::fmod(sourceY, 256.0));
			const float z = static_cast<float>(std::fmod(sourceZ, 256.0));
			const float floorX = std::floor(x);
			const float floorY = std::floor(y);
			const float floorZ = std::floor(z);
			const int xi = static_cast<int>(floorX) & 255;
			const int yi = static_cast<int>(floorY) & 255;
			const int zi = static_cast<int>(floorZ) & 255;
			const float xf = x - floorX;
			const float yf = y - floorY;
			const float zf = z - floorZ;
			const float u = Fade(xf);
			const float v = Fade(yf);
			const float w = Fade(zf);
			const int a = (PERLIN_HASH[xi] + yi) & 255;
			const int aa = (PERLIN_HASH[a] + zi) & 255;
			const int ab = (PERLIN_HASH[a + 1] + zi) & 255;
			const int b = (PERLIN_HASH[xi + 1] + yi) & 255;
			const int ba = (PERLIN_HASH[b] + zi) & 255;
			const int bb = (PERLIN_HASH[b + 1] + zi) & 255;
			const float lowA =
				Lerp(u, Gradient(PERLIN_HASH[aa], xf, yf, zf), Gradient(PERLIN_HASH[ba], xf - 1, yf, zf));
			const float lowB = Lerp(
				u, Gradient(PERLIN_HASH[ab], xf, yf - 1, zf), Gradient(PERLIN_HASH[bb], xf - 1, yf - 1, zf)
			);
			const float highA = Lerp(
				u,
				Gradient(PERLIN_HASH[aa + 1], xf, yf, zf - 1),
				Gradient(PERLIN_HASH[ba + 1], xf - 1, yf, zf - 1)
			);
			const float highB = Lerp(
				u,
				Gradient(PERLIN_HASH[ab + 1], xf, yf - 1, zf - 1),
				Gradient(PERLIN_HASH[bb + 1], xf - 1, yf - 1, zf - 1)
			);
			return Lerp(w, Lerp(v, lowA, lowB), Lerp(v, highA, highB));
		}

		bool Valid(const NoiseGridRequest &request, std::string &error) {
			const uint64_t samples = static_cast<uint64_t>(request.Width) * request.Depth;
			if (request.Width == 0 || request.Depth == 0) {
				error = "noise grid dimensions must be positive integers";
				return false;
			}
			if (samples > ComputeJobs::MAXIMUM_SAMPLES) {
				error = "noise grid exceeds the 1048576 sample limit";
				return false;
			}
			if (!std::isfinite(request.OriginX) || !std::isfinite(request.OriginY) ||
				!std::isfinite(request.OriginZ) || !std::isfinite(request.Step)) {
				error = "noise grid coordinates and step must be finite";
				return false;
			}
			return true;
		}

		bool BuildNoise(const NoiseGridRequest &request, std::stop_token stop, std::vector<float> &values) {
			values.resize(static_cast<size_t>(request.Width) * request.Depth);
			for (uint32_t row = 0; row < request.Depth; row++) {
				if (stop.stop_requested()) {
					values.clear();
					return false;
				}
				const double z = request.OriginZ + static_cast<double>(row) * request.Step;
				for (uint32_t column = 0; column < request.Width; column++) {
					if ((column & 255u) == 0 && stop.stop_requested()) {
						values.clear();
						return false;
					}
					const double x = request.OriginX + static_cast<double>(column) * request.Step;
					values[static_cast<size_t>(row) * request.Width + column] = Noise(x, request.OriginY, z);
				}
			}
			return true;
		}

		void BuildNoiseSlice(
			const NoiseGridRequest &request, size_t begin, size_t end, std::vector<float> &values
		) {
			const size_t count = static_cast<size_t>(request.Width) * request.Depth;
			if (values.size() != count) {
				values.resize(count);
			}
			for (size_t index = begin; index < end; index++) {
				const uint32_t row = static_cast<uint32_t>(index / request.Width);
				const uint32_t column = static_cast<uint32_t>(index % request.Width);
				const double x = request.OriginX + static_cast<double>(column) * request.Step;
				const double z = request.OriginZ + static_cast<double>(row) * request.Step;
				values[index] = Noise(x, request.OriginY, z);
			}
		}

		std::vector<std::byte> EncodeRequest(const NoiseGridRequest &request) {
			core::ByteWriter writer;
			writer.WriteUInt32(PROTOCOL_MAGIC);
			writer.WriteUInt16(PROTOCOL_VERSION);
			writer.WriteUInt32(request.Width);
			writer.WriteUInt32(request.Depth);
			writer.WriteDouble(request.OriginX);
			writer.WriteDouble(request.OriginY);
			writer.WriteDouble(request.OriginZ);
			writer.WriteDouble(request.Step);
			return {writer.Bytes().begin(), writer.Bytes().end()};
		}

		bool DecodeRequest(std::span<const std::byte> frame, NoiseGridRequest &request) {
			core::ByteReader reader(frame);
			if (reader.ReadUInt32() != PROTOCOL_MAGIC || reader.ReadUInt16() != PROTOCOL_VERSION) {
				return false;
			}
			request.Width = reader.ReadUInt32();
			request.Depth = reader.ReadUInt32();
			request.OriginX = reader.ReadDouble();
			request.OriginY = reader.ReadDouble();
			request.OriginZ = reader.ReadDouble();
			request.Step = reader.ReadDouble();
			std::string error;
			return reader.AtEnd() && Valid(request, error);
		}

		std::vector<std::byte> EncodeResult(std::span<const float> values) {
			core::ByteWriter writer(sizeof(uint32_t) + values.size() * sizeof(float));
			writer.WriteUInt32(static_cast<uint32_t>(values.size()));
			for (float value : values) {
				writer.WriteFloat(value);
			}
			return {writer.Bytes().begin(), writer.Bytes().end()};
		}

		bool DecodeResult(std::span<const std::byte> frame, size_t expected, std::vector<float> &values) {
			core::ByteReader reader(frame);
			const uint32_t count = reader.ReadUInt32();
			if (count != expected || reader.Remaining() != static_cast<size_t>(count) * sizeof(float)) {
				return false;
			}
			values.resize(count);
			for (float &value : values) {
				value = reader.ReadFloat();
			}
			return reader.AtEnd();
		}
	}

	struct ComputeJobs::State {
		struct Job {
			uint64_t Ticket = 0;
			NoiseGridRequest Request;
			parallel::JobContext Context = parallel::JobContext::Serial;
			std::vector<float> Values;
			std::vector<float> FallbackValues;
			size_t FallbackCursor = 0;
			std::string Error;
			std::atomic<WorkState> Status{WorkState::Pending};
			std::jthread Thread;
			parallel::Process Process;
			std::unique_ptr<parallel::Channel> Channel;
			std::vector<std::byte> Frame;
			uint64_t ReadyPoll = 0;
			bool Published = false;
		};

		uint64_t NextTicket = 0;
		uint64_t NextPublishTicket = 1;
		uint64_t PollNumber = 0;
		std::vector<std::unique_ptr<Job>> Active;
		std::vector<ComputeCompletion> Completed;
		std::string Error;
	};

	ComputeJobs::ComputeJobs() : Held(std::make_unique<State>()) {}

	ComputeJobs::~ComputeJobs() {
		for (const std::unique_ptr<State::Job> &job : Held->Active) {
			if (job->Thread.joinable()) {
				job->Thread.request_stop();
			}
			if (job->Channel) {
				job->Channel->Close();
			}
		}
		Held->Active.clear();
	}

	uint64_t ComputeJobs::SubmitNoise(const NoiseGridRequest &request, parallel::JobContext context) {
		Held->Error.clear();
		if (!Valid(request, Held->Error)) {
			return 0;
		}
		if (Held->Active.size() >= MAXIMUM_PENDING) {
			Held->Error = "compute queue is full";
			return 0;
		}

		auto job = std::make_unique<State::Job>();
		job->Ticket = ++Held->NextTicket;
		job->Request = request;
		job->Context = context;
		const size_t sampleCount = static_cast<size_t>(request.Width) * request.Depth;
		const uint64_t beats =
			static_cast<uint64_t>((sampleCount + SAMPLES_PER_HEARTBEAT - 1) / SAMPLES_PER_HEARTBEAT);
		job->ReadyPoll = Held->PollNumber + std::max<uint64_t>(beats, 1);
		State::Job *owned = job.get();

		if (context == parallel::JobContext::Serial) {
			BuildNoise(request, {}, job->Values);
			job->Status.store(WorkState::Complete, std::memory_order_release);
		} else if (context == parallel::JobContext::Threaded) {
			job->Thread = std::jthread([owned](std::stop_token stop) {
				const bool complete = BuildNoise(owned->Request, stop, owned->Values);
				owned->Status.store(
					complete ? WorkState::Complete : WorkState::Cancelled, std::memory_order_release
				);
			});
		} else {
			WorkerConfiguration worker;
			{
				std::lock_guard lock(WorkerProgramGuard());
				worker = Worker();
			}
			if (worker.Program.empty()) {
				Held->Error =
					"processed compute is unavailable because the program did not configure a worker";
				return 0;
			}

			parallel::ChannelSettings settings;
			settings.MaximumFrame = sizeof(uint32_t) + MAXIMUM_SAMPLES * sizeof(float);
			settings.Capacity = settings.MaximumFrame * 2;
			parallel::ProcessChannel pair = parallel::MakeProcessChannel(settings);
			if (!pair.Valid() ||
				!job->Process.Start(worker.Program, worker.Arguments, std::move(pair.Remote))) {
				Held->Error = "processed compute worker could not be started";
				return 0;
			}
			job->Channel = std::move(pair.Local);
			const std::vector<std::byte> frame = EncodeRequest(request);
			if (job->Channel->Send(frame) != parallel::ChannelStatus::Ok) {
				Held->Error = "processed compute request could not be sent";
				job->Channel->Close();
				return 0;
			}
		}

		const uint64_t ticket = job->Ticket;
		Held->Active.push_back(std::move(job));
		return ticket;
	}

	void ComputeJobs::Poll() {
		Held->Completed.clear();
		Held->PollNumber++;
		for (const std::unique_ptr<State::Job> &job : Held->Active) {
			if (job->Context == parallel::JobContext::Processed &&
				job->Status.load(std::memory_order_acquire) == WorkState::Pending) {
				const parallel::ChannelStatus received = job->Channel->Receive(job->Frame);
				if (received == parallel::ChannelStatus::Ok) {
					const size_t expected = static_cast<size_t>(job->Request.Width) * job->Request.Depth;
					if (DecodeResult(job->Frame, expected, job->Values)) {
						const std::array<std::byte, 1> acknowledgement{std::byte{1}};
						(void)job->Channel->Send(acknowledgement);
						job->Status.store(WorkState::Complete, std::memory_order_release);
					} else {
						job->Error = "processed compute worker returned a malformed result";
						job->Status.store(WorkState::Failed, std::memory_order_release);
					}
				} else if (received == parallel::ChannelStatus::Closed || job->Process.Poll().Faulted()) {
					job->Error = "processed compute worker exited before returning a result";
					job->Status.store(WorkState::Failed, std::memory_order_release);
				}
			}

			const WorkState observed = job->Status.load(std::memory_order_acquire);
			const size_t sampleCount = static_cast<size_t>(job->Request.Width) * job->Request.Depth;
			if (!job->Published && observed == WorkState::Pending && job->FallbackCursor < sampleCount) {
				const size_t end = std::min(job->FallbackCursor + SAMPLES_PER_HEARTBEAT, sampleCount);
				BuildNoiseSlice(job->Request, job->FallbackCursor, end, job->FallbackValues);
				job->FallbackCursor = end;
			}

			if (job->Published || job->Ticket != Held->NextPublishTicket ||
				Held->PollNumber < job->ReadyPoll) {
				continue;
			}

			if (observed == WorkState::Failed) {
				Held->Completed.push_back(ComputeCompletion{job->Ticket, {}, job->Error});
			} else if (observed == WorkState::Complete) {
				Held->Completed.push_back(ComputeCompletion{job->Ticket, std::move(job->Values), {}});
			} else if (job->FallbackCursor == sampleCount) {
				Held->Completed.push_back(ComputeCompletion{job->Ticket, std::move(job->FallbackValues), {}});
				if (job->Thread.joinable()) {
					job->Thread.request_stop();
				}
				if (job->Channel) {
					job->Channel->Close();
				}
			} else {
				continue;
			}
			job->Published = true;
			Held->NextPublishTicket++;
		}

		std::erase_if(Held->Active, [](const std::unique_ptr<State::Job> &job) {
			return job->Published && job->Status.load(std::memory_order_acquire) != WorkState::Pending;
		});
	}

	std::span<const ComputeCompletion> ComputeJobs::Completions() const {
		return Held->Completed;
	}

	void ComputeJobs::ClearCompletions() {
		Held->Completed.clear();
	}

	size_t ComputeJobs::PendingCount() const {
		return static_cast<size_t>(std::count_if(
			Held->Active.begin(), Held->Active.end(), [](const std::unique_ptr<State::Job> &job) {
				return !job->Published;
			}
		));
	}

	const std::string &ComputeJobs::LastError() const {
		return Held->Error;
	}

	void
	ConfigureComputeWorkerProgram(const std::filesystem::path &program, std::vector<std::string> arguments) {
		std::lock_guard lock(WorkerProgramGuard());
		if (program.empty()) {
			Worker() = {};
			return;
		}
		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(program, error);
		Worker().Program = error ? program : absolute;
		Worker().Arguments = std::move(arguments);
	}

	bool ComputeWorkerRequested(int argc, char **argv) {
		return argc == 2 && argv != nullptr && argv[1] != nullptr && argv[1] == WORKER_ARGUMENT;
	}

	int RunComputeWorker() {
		parallel::ChannelSettings settings;
		settings.MaximumFrame = sizeof(uint32_t) + ComputeJobs::MAXIMUM_SAMPLES * sizeof(float);
		settings.Capacity = settings.MaximumFrame * 2;
		std::unique_ptr<parallel::Channel> channel = parallel::AdoptInheritedChannel(settings);
		if (!channel) {
			return 2;
		}

		std::vector<std::byte> frame;
		NoiseGridRequest request;
		for (;;) {
			const parallel::ChannelStatus received = channel->Receive(frame);
			if (received == parallel::ChannelStatus::Closed) {
				return 1;
			}
			if (received == parallel::ChannelStatus::Ok) {
				if (!DecodeRequest(frame, request)) {
					return 2;
				}
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		std::vector<float> values;
		BuildNoise(request, {}, values);
		const std::vector<std::byte> result = EncodeResult(values);
		if (channel->Send(result) != parallel::ChannelStatus::Ok) {
			return 2;
		}

		for (;;) {
			const parallel::ChannelStatus received = channel->Receive(frame);
			if (received == parallel::ChannelStatus::Ok) {
				return 0;
			}
			if (received == parallel::ChannelStatus::Closed) {
				return 1;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}
