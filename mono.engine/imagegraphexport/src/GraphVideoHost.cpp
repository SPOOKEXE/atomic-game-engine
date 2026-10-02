#include <engine/bake/Image.hpp>
#include <engine/imagegraphexport/GraphVideoHost.hpp>
#include <engine/parallel/Process.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <thread>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		constexpr uint64_t MAXIMUM_CLIP_BYTES = 64ull * 1024 * 1024;
		const Value *Input(const HostNodeInvocation &invocation, std::string_view port) {
			const auto found =
				std::find_if(invocation.Inputs.begin(), invocation.Inputs.end(), [&](const auto &value) {
					return value.Port == port;
				});
			return found == invocation.Inputs.end() ? nullptr : &found->Data;
		}
		template <class T> const T *Get(const HostNodeInvocation &invocation, std::string_view port) {
			const auto *value = Input(invocation, port);
			return value ? std::get_if<T>(value) : nullptr;
		}
		bool Decode(
			const GraphVideoSettings &settings,
			const std::filesystem::path &path,
			uint64_t maximum,
			std::vector<Image> &frames,
			std::string &failure
		) {
			std::error_code error;
			if (settings.Decoder.empty() || !settings.Decoder.is_absolute() ||
				!std::filesystem::is_regular_file(settings.Decoder, error) || settings.Timeout.count() <= 0 ||
				settings.Timeout > std::chrono::minutes(10)) {
				failure = "video input needs an explicitly granted absolute decoder and bounded deadline";
				return false;
			}
			const auto source = std::filesystem::weakly_canonical(path, error);
			if (error || !std::filesystem::is_regular_file(source, error)) {
				failure = "video input is not a regular explicit file";
				return false;
			}
			const auto size = std::filesystem::file_size(source, error);
			if (error || size == 0 || size > MAXIMUM_CLIP_BYTES) {
				failure = "video source size exceeds its input budget";
				return false;
			}
			std::array<char, 12> signature{};
			std::ifstream sourceHeader(source, std::ios::binary);
			sourceHeader.read(signature.data(), signature.size());
			const auto observed = static_cast<size_t>(sourceHeader.gcount());
			const std::string_view header(signature.data(), observed);
			std::string demuxer;
			if (header.starts_with("GIF87a") || header.starts_with("GIF89a"))
				demuxer = "gif";
			else if (header.size() >= 12 && header.substr(4, 4) == "ftyp")
				demuxer = "mov";
			else {
				failure = "video source must contain a native MP4 or GIF header";
				return false;
			}
			static std::atomic<uint64_t> sequence{0};
			const auto directory =
				std::filesystem::temp_directory_path() /
				("atomic-video-" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
				 std::to_string(sequence.fetch_add(1)));
			if (!std::filesystem::create_directory(directory, error) || error) {
				failure = "cannot create exclusive decoder staging directory";
				return false;
			}
			struct Cleanup {
				std::filesystem::path Path;
				~Cleanup() {
					std::error_code error;
					std::filesystem::remove_all(Path, error);
				}
			} cleanup{directory};
			std::vector<std::string> arguments{
				"-hide_banner",
				"-loglevel",
				"error",
				"-nostdin",
				"-max_alloc",
				"67108864",
				"-protocol_whitelist",
				"file",
				"-f",
				demuxer,
				"-i",
				source.string(),
				"-map",
				"0:v:0",
				"-an",
				"-sn",
				"-dn",
				"-pix_fmt",
				"rgba",
				"-vsync",
				"0",
				"-frames:v",
				"4097",
				"-start_number",
				"0",
				(directory / "frame%08d.png").string()
			};
			engine::parallel::Process process;
			if (!process.Start(settings.Decoder, arguments)) {
				failure = "cannot start explicit video decoder";
				return false;
			}
			const auto deadline = std::chrono::steady_clock::now() + settings.Timeout;
			while (true) {
				const auto state = process.Poll();
				uint64_t total = 0;
				size_t count = 0;
				for (std::filesystem::directory_iterator iterator(directory, error), end;
					 !error && iterator != end;
					 iterator.increment(error)) {
					const auto bytes = iterator->file_size(error);
					if (error) break;
					if (bytes > MAXIMUM_CLIP_BYTES - total) {
						failure = "decoded video files exceed their byte budget";
						break;
					}
					total += bytes;
					count++;
					if (count > Limits::MaximumRangeFrames) {
						failure = "video contains more than 4096 frames";
						break;
					}
				}
				if (error) failure = "cannot inspect decoded video frames";
				if (!failure.empty() || std::chrono::steady_clock::now() >= deadline) {
					(void)process.Kill();
					(void)process.Wait();
					if (failure.empty()) failure = "video decoder exceeded its deadline";
					return false;
				}
				if (!state.Alive()) {
					if (state.Reason != engine::parallel::ExitReason::Exited || state.Code != 0) {
						failure = "video decoder failed";
						return false;
					}
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
			std::vector<std::filesystem::path> files;
			for (const auto &entry : std::filesystem::directory_iterator(directory, error))
				files.push_back(entry.path());
			if (error || files.empty() || files.size() > Limits::MaximumRangeFrames) {
				failure = "video decoder produced no bounded frame sequence";
				return false;
			}
			std::sort(files.begin(), files.end());
			uint64_t retained = 0;
			std::vector<Image> decoded;
			if (files.size() * sizeof(Image) > maximum) {
				failure = "video frame count exceeds its shape budget";
				return false;
			}
			decoded.reserve(files.size());
			for (size_t index = 0; index < files.size(); index++) {
				const auto size = std::filesystem::file_size(files[index], error);
				if (error || size < 24 || size > MAXIMUM_CLIP_BYTES || size > maximum - retained) {
					failure = "invalid decoded video frame size";
					return false;
				}
				std::vector<std::byte> bytes(static_cast<size_t>(size));
				std::ifstream stream(files[index], std::ios::binary);
				stream.read(
					reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
				);
				if (!stream || engine::bake::ImageFormatOfBytes(bytes) != engine::bake::ImageFormat::Png) {
					failure = "video decoder output is not a complete PNG frame";
					return false;
				}
				uint32_t width = 0, height = 0;
				for (size_t offset = 0; offset < 4; offset++) {
					width = (width << 8) | std::to_integer<uint8_t>(bytes[16 + offset]);
					height = (height << 8) | std::to_integer<uint8_t>(bytes[20 + offset]);
				}
				const uint64_t pixels = uint64_t{width} * height * 4;
				if (width == 0 || height == 0 || width > Limits::MaximumDimension ||
					height > Limits::MaximumDimension || pixels > maximum - retained ||
					size > maximum - retained - pixels) {
					failure = "video frame dimensions or decoded clip exceed their operation budget";
					return false;
				}
				engine::assets::TextureData texture;
				if (!engine::bake::ReadImage(bytes, texture, failure)) return false;
				if (!decoded.empty() &&
					(texture.Width != decoded[0].Width || texture.Height != decoded[0].Height)) {
					failure = "video frame dimensions changed within one clip";
					return false;
				}
				Image image;
				image.Width = texture.Width;
				image.Height = texture.Height;
				image.Pixels.reserve(texture.Pixels.size());
				for (auto byte : texture.Pixels)
					image.Pixels.push_back(std::to_integer<uint8_t>(byte));
				image.Hash = SurfaceHash(image);
				retained += image.Pixels.size();
				decoded.push_back(std::move(image));
			}
			frames = std::move(decoded);
			return true;
		}
	}
	GraphVideoHost::GraphVideoHost(GraphVideoSettings settings) : Settings(std::move(settings)) {}
	bool GraphVideoHost::Capture(
		const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
	) {
		failure.clear();
		const auto &type = invocation.Authored.Type;
		if (type != "pc.image_mp4" && type != "pc.image_gif") {
			failure = "video host does not support this node type";
			return false;
		}
		const auto *path = Get<std::string>(invocation, "path");
		const GraphVideoGrant *grant = nullptr;
		for (const auto &candidate : Settings.Grants)
			if (candidate.NodeId == invocation.Authored.Id) {
				if (grant) {
					failure = "video node grants are duplicated";
					return false;
				}
				grant = &candidate;
			}
		if (!grant || !path || *path != grant->File.string() || !Settings.Content.AllowsName(*path)) {
			failure = "video node requires an exact path grant and content policy";
			return false;
		}
		auto clip = std::find_if(Clips.begin(), Clips.end(), [&](const Clip &clip) {
			return clip.NodeId == invocation.Authored.Id && clip.Path == grant->File;
		});
		if (clip == Clips.end()) {
			uint64_t retained = 0;
			for (const auto &cached : Clips)
				for (const auto &frame : cached.Frames)
					retained += frame.Pixels.size();
			if (retained >= MAXIMUM_CLIP_BYTES) {
				failure = "decoded clip cache exceeds its byte budget";
				return false;
			}
			Clip decoded;
			decoded.NodeId = invocation.Authored.Id;
			decoded.Path = grant->File;
			if (!Decode(
					Settings,
					grant->File,
					std::min(MAXIMUM_CLIP_BYTES - retained, invocation.MaximumOperationBytes / 4),
					decoded.Frames,
					failure
				))
				return false;
			Clips.push_back(std::move(decoded));
			clip = std::prev(Clips.end());
		}
		const auto *array = Get<bool>(invocation, "output_as_array"),
				   *custom = Get<bool>(invocation, "custom_frame_order"),
				   *before = Get<bool>(invocation, "draw_before_start");
		const auto *start = Get<int64_t>(invocation, "start_frame"),
				   *frame = Get<int64_t>(invocation, "frame");
		const auto *speed = Get<double>(invocation, "animation_speed");
		const auto *loop = Get<EnumValue>(invocation, "loop_mode");
		if (!array || !custom || !before || !start || !frame || !speed || !loop || !std::isfinite(*speed) ||
			loop->Value < 0 || loop->Value > 3 || clip->Frames.empty()) {
			failure = "video animation controls have unsupported types or bounds";
			return false;
		}
		HostNodeCapture capture;
		capture.Authored = invocation.Authored;
		capture.Tick = invocation.Request.Tick;
		capture.Subframe = invocation.Request.Subframe;
		capture.NegativeFrame = invocation.Request.NegativeFrame;
		capture.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
		const auto &first = clip->Frames[0];
		capture.Outputs = {
			{"path", *path},
			{"dimension", Vector2{static_cast<double>(first.Width), static_cast<double>(first.Height)}}
		};
		uint64_t bytes = 0;
		for (const auto &image : clip->Frames)
			bytes += image.Pixels.size();
		if ((*array ? bytes : first.Pixels.size()) > invocation.MaximumOperationBytes / 2) {
			failure = "video observation exceeds its operation budget";
			return false;
		}
		if (*array)
			capture.ImageArrays.push_back({"surface_out", clip->Frames});
		else {
			double position =
				*custom ? static_cast<double>(*frame)
						: ((invocation.Request.NegativeFrame ? -1.0 : 1.0) *
						   (static_cast<double>(invocation.Request.Tick) + invocation.Request.Subframe)) *
								  *speed -
							  (static_cast<double>(*start) - 1);
			if (!std::isfinite(position)) {
				failure = "video frame selection is not finite";
				return false;
			}
			bool draw = *before || position >= 0;
			const double count = static_cast<double>(clip->Frames.size());
			switch (loop->Value) {
			case 0:
				position = std::fmod(std::fmod(position, count) + count, count);
				break;
			case 1:
				if (count == 1)
					position = 0;
				else {
					const double period = count * 2 - 2;
					position = std::fmod(std::fmod(position, period) + period, period);
					if (position >= count) position = period - position;
				}
				break;
			case 2:
				position = std::clamp(position, 0.0, count - 1);
				break;
			case 3:
				if (position < 0 || position >= count) draw = false;
				break;
			}
			Image image =
				draw ? clip->Frames[static_cast<size_t>(std::clamp(position, 0.0, count - 1))] : first;
			if (!draw) {
				std::fill(image.Pixels.begin(), image.Pixels.end(), 0);
				image.Hash = SurfaceHash(image);
			}
			capture.Images.push_back({"surface_out", std::move(image)});
		}
		output = std::move(capture);
		return true;
	}
}
