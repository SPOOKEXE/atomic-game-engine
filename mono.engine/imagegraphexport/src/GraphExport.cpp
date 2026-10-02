#include <engine/bake/GifWrite.hpp>
#include <engine/bake/Image.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphexport/GraphInputs.hpp>
#include <engine/imagegraphexport/Runner.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/parallel/Process.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <thread>

namespace engine::imagegraphexport {
	namespace {
		constexpr uint64_t MAXIMUM_EXPORT_BYTES = 512ull * 1024 * 1024;
		std::string Number(double value) {
			std::ostringstream text;
			text.imbue(std::locale::classic());
			text << std::setprecision(17) << value;
			return text.str();
		}
		std::filesystem::path FrameName(const std::filesystem::path &directory, size_t index) {
			std::ostringstream name;
			name << "frame" << std::setfill('0') << std::setw(8) << index << ".png";
			return directory / name.str();
		}
		bool BoundedImageHeader(std::span<const std::byte> bytes, uint64_t remaining, std::string &failure) {
			const auto byte = [&](size_t index) { return std::to_integer<uint8_t>(bytes[index]); };
			uint32_t width = 0, height = 0;
			const auto format = engine::bake::ImageFormatOfBytes(bytes);
			if (format == engine::bake::ImageFormat::Png && bytes.size() >= 24) {
				for (size_t index = 0; index < 4; index++) {
					width = (width << 8) | byte(16 + index);
					height = (height << 8) | byte(20 + index);
				}
			} else if (format == engine::bake::ImageFormat::Bmp && bytes.size() >= 26) {
				uint32_t rawHeight = 0;
				for (size_t index = 0; index < 4; index++) {
					width |= uint32_t{byte(18 + index)} << (index * 8);
					rawHeight |= uint32_t{byte(22 + index)} << (index * 8);
				}
				height = rawHeight & 0x80000000u ? 0u - rawHeight : rawHeight;
			} else if (format == engine::bake::ImageFormat::Jpeg) {
				size_t cursor = 2;
				while (cursor < bytes.size()) {
					if (byte(cursor++) != 255) break;
					while (cursor < bytes.size() && byte(cursor) == 255)
						cursor++;
					if (cursor >= bytes.size()) break;
					const unsigned marker = byte(cursor++);
					if (marker == 0xd9 || marker == 0xda) break;
					if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
					if (bytes.size() - cursor < 2) break;
					const size_t length = (size_t{byte(cursor)} << 8) | byte(cursor + 1);
					if (length < 2 || length > bytes.size() - cursor) break;
					if (marker == 0xc0 && length >= 8) {
						height = (uint32_t{byte(cursor + 3)} << 8) | byte(cursor + 4);
						width = (uint32_t{byte(cursor + 5)} << 8) | byte(cursor + 6);
						break;
					}
					cursor += length;
				}
			}
			const uint64_t decoded = uint64_t{width} * height * 4;
			if (width == 0 || height == 0 || width > engine::imagegraph::Limits::MaximumDimension ||
				height > engine::imagegraph::Limits::MaximumDimension ||
				decoded > engine::imagegraph::Limits::MaximumOutputBytes || decoded > remaining) {
				failure = "static input header is unsupported or exceeds its decode budget";
				return false;
			}
			return true;
		}

		struct Stage {
			std::filesystem::path Directory;
			bool Retain = false;
			~Stage() {
				if (Directory.empty() || Retain) return;
				std::error_code error;
				std::filesystem::remove_all(Directory, error);
			}
		};
	}

	bool BuildGraphEncoderArguments(
		const GraphExportSettings &settings,
		const std::filesystem::path &frames,
		const std::filesystem::path &target,
		size_t frameCount,
		std::filesystem::path &executable,
		std::vector<std::string> &arguments,
		std::string &failure
	) {
		arguments.clear();
		executable.clear();
		if (frameCount == 0 || frameCount > 4096 || settings.GifBatchSize > 4096 ||
			settings.FrameMilliseconds == 0 || settings.Quality > 100 ||
			!std::isfinite(settings.MegabitsPerSecond) || settings.MegabitsPerSecond <= 0 ||
			settings.MegabitsPerSecond > 1000 || settings.EncoderTimeout.count() <= 0 ||
			settings.EncoderTimeout > std::chrono::minutes(10)) {
			failure = "export timing, quality, count or bitrate is outside its bounds";
			return false;
		}
		const double quality = settings.EncoderQuality.value_or(settings.Quality);
		const auto fps = settings.EncoderFramesPerSecond;
		if (!std::isfinite(quality) || quality < 0 || quality > 100 ||
			(fps && (!std::isfinite(*fps) || *fps <= 0 || *fps > 1000000)) ||
			!std::isfinite(settings.GifColorMerge) || settings.GifColorMerge < 0 ||
			settings.GifColorMerge > 1) {
			failure = "encoder precision controls exceed their bounds";
			return false;
		}
		const std::string extension = settings.Output.extension().string();
		if (settings.Animation && (extension == ".mp4" || extension == ".webm")) {
			executable = settings.VideoEncoder;
			arguments = {
				"-hide_banner",
				"-loglevel",
				"error",
				"-nostdin",
				"-framerate",
				Number(fps.value_or(1000.0 / settings.FrameMilliseconds)),
				"-start_number",
				"0",
				"-i",
				(frames / "frame%08d.png").string(),
				"-frames:v",
				std::to_string(frameCount)
			};
			if (extension == ".mp4") {
				arguments.insert(
					arguments.end(),
					{"-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", Number(std::min(quality, 51.0))}
				);
			} else {
				arguments.insert(
					arguments.end(),
					{"-c:v",
					 "libvpx-vp9",
					 "-pix_fmt",
					 "yuva420p",
					 "-b:v",
					 Number(settings.MegabitsPerSecond) + "M",
					 "-crf",
					 Number(std::min(quality, 63.0)),
					 "-deadline",
					 "good",
					 "-auto-alt-ref",
					 "0"}
				);
			}
			arguments.insert(arguments.end(), {"-y", target.string()});
		} else if ((settings.Animation && (extension == ".gif" || extension == ".webp")) ||
				   (!settings.Animation &&
					(extension == ".jpg" || extension == ".jpeg" || extension == ".webp" ||
					 extension == ".ico" || extension == ".txt" ||
					 (extension == ".png" && settings.PngSubformat < 2)))) {
			executable = settings.ImageEncoder;
			if (settings.Animation)
				arguments = {
					"-delay",
					fps ? "1x" + Number(*fps) : Number(settings.FrameMilliseconds / 10.0),
					"-dispose",
					"2",
					"-loop",
					std::to_string(settings.Plays)
				};
			for (size_t index = 0; index < frameCount; index++)
				arguments.push_back(FrameName(frames, index).string());
			if (settings.Animation && extension == ".gif") {
				if (settings.OptimiseGif)
					arguments.insert(
						arguments.end(),
						{"-fuzz",
						 Number(settings.GifColorMerge * 100) + "%",
						 "-layers",
						 "OptimizeFrame",
						 "-layers",
						 "OptimizeTransparency"}
					);
				arguments.insert(arguments.end(), {"-alpha", "set"});
			}
			if (extension == ".webp") arguments.insert(arguments.end(), {"-define", "webp:lossless=true"});
			if (!settings.Animation) arguments.insert(arguments.end(), {"-quality", Number(quality)});
			arguments.push_back(
				extension == ".png" && settings.PngSubformat == 1 ? "PNG8:" + target.string()
																  : target.string()
			);
		} else {
			failure = "requested format and animation mode have no external encoder route";
			return false;
		}
		if (executable.empty() || !executable.is_absolute()) {
			failure = "external format requires an explicit absolute encoder executable path";
			return false;
		}
		return true;
	}

	bool LoadGraphImageInputs(
		const GraphExportSettings &settings,
		std::vector<engine::imagegraph::RequestImageSource> &imageSources,
		std::string &failure,
		uint64_t maximumBytes
	) {
		imageSources.clear();
		std::error_code error;
		uint64_t imageBytes = 0;
		if (settings.ImageInputs.size() > engine::imagegraph::Limits::MaximumNodes) {
			failure = "image source count exceeds its budget";
			return false;
		}
		for (const auto &source : settings.ImageInputs) {
			if (!settings.Content.AllowsName(source.File.string())) {
				failure = "image source is refused by content policy";
				return false;
			}
			if (source.SourceId.empty() || source.SourceId.size() > 255 ||
				std::any_of(imageSources.begin(), imageSources.end(), [&](const auto &known) {
					return known.SourceId == source.SourceId;
				})) {
				failure = "image source ID is empty, too long or duplicated";
				return false;
			}
			const uint64_t sourceBytes = std::filesystem::file_size(source.File, error);
			if (error || sourceBytes == 0 || sourceBytes > 64ull * 1024 * 1024 ||
				sourceBytes > maximumBytes - imageBytes) {
				failure = "image input exceeds its byte budget or cannot be inspected";
				return false;
			}
			std::vector<std::byte> bytes(static_cast<size_t>(sourceBytes));
			std::ifstream stream(source.File, std::ios::binary);
			stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!stream || static_cast<size_t>(stream.gcount()) != bytes.size()) {
				failure = "could not read complete explicit image input";
				return false;
			}
			if (!BoundedImageHeader(bytes, (maximumBytes - imageBytes - sourceBytes) / 2, failure))
				return false;
			engine::assets::TextureData texture;
			if (!engine::bake::ReadImage(bytes, texture, failure)) return false;
			if (texture.Format != engine::assets::TextureFormat::RGBA8 || texture.FlipbookFrames != 0) {
				failure = "static image inputs require RGBA8 images; animated inputs need an explicit frame "
						  "adapter";
				return false;
			}
			const uint64_t decodedBytes = texture.Pixels.size();
			if (decodedBytes > maximumBytes - imageBytes) {
				failure = "decoded image inputs exceed their aggregate byte budget";
				return false;
			}
			engine::imagegraph::Image image;
			image.Width = texture.Width;
			image.Height = texture.Height;
			image.Pixels.reserve(texture.Pixels.size());
			for (std::byte byte : texture.Pixels)
				image.Pixels.push_back(std::to_integer<uint8_t>(byte));
			image.Hash = engine::imagegraph::SurfaceHash(image);
			imageBytes += decodedBytes;
			imageSources.push_back({source.SourceId, std::move(image)});
		}
		return true;
	}

	static bool ExportGraphImpl(
		const GraphExportSettings &settings,
		std::string &failure,
		std::filesystem::path *retainedTemporaryDirectory,
		const engine::imagegraph::Document *liveDocument,
		const engine::imagegraph::Plan *livePlan,
		const engine::imagegraph::EvaluationRequest *liveRequest
	) {
		failure.clear();
		size_t frameCount = 0;
		engine::imagegraph::Diagnostic diagnostic;
		if (engine::imagegraph::ValidateTickRange(settings.Frames, frameCount, diagnostic) !=
			engine::imagegraph::Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		if (!settings.Content.AllowsName(settings.Input.string()) ||
			!settings.Content.AllowsName(settings.Output.string())) {
			failure = "graph input is refused by content policy";
			return false;
		}
		if (settings.Input.empty() || settings.Output.empty() || settings.OutputId.empty() ||
			(!settings.Animation && frameCount != 1)) {
			failure =
				"export requires graph, output, durable output ID and one still frame or an animation range";
			return false;
		}
		std::error_code error;
		const auto input = std::filesystem::weakly_canonical(settings.Input, error);
		if (error) {
			failure = "cannot resolve input graph";
			return false;
		}
		const auto output = std::filesystem::weakly_canonical(settings.Output, error);
		if (error || input == output) {
			failure = "input graph and output must be different valid paths";
			return false;
		}
		const auto parent = output.parent_path();
		std::filesystem::create_directories(parent, error);
		if (error) {
			failure = "cannot create export parent directory";
			return false;
		}
		static std::atomic<uint64_t> sequence{0};
		Stage stage;
		const auto stagingDirectory =
			parent /
			(".graph-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
			 "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
		if (settings.RetainTemporaryFrames && !settings.Content.AllowsName(stagingDirectory.string())) {
			failure = "Retained temporary frame destination is refused by content policy";
			return false;
		}
		if (!std::filesystem::create_directory(stagingDirectory, error) || error) {
			failure = "cannot create exclusive export staging directory";
			return false;
		}
		stage.Directory = stagingDirectory;
		const auto target = stage.Directory / output.filename();
		const auto extension = output.extension().string();
		const bool nativeGif = settings.NativeGif && extension == ".gif" && settings.Animation;
		if (settings.NativeGif && (!nativeGif || settings.NativeGifQuality > 3)) {
			failure = "Native GIF profile requires animated .gif output and quantization 0..3";
			return false;
		}
		const bool native = nativeGif || (extension == ".png" && settings.PngSubformat == 2) ||
							extension == ".bmp" || extension == ".exr" || extension == ".apng";
		if (native && settings.Animation != (extension == ".apng" || nativeGif)) {
			failure = "native animation export uses .apng";
			return false;
		}
		std::filesystem::path encoder;
		std::vector<std::string> encoderArguments;
		if (!native && !BuildGraphEncoderArguments(
						   settings, stage.Directory, target, frameCount, encoder, encoderArguments, failure
					   ))
			return false;
		if (!native && !std::filesystem::is_regular_file(encoder, error)) {
			failure = "encoder executable does not exist";
			return false;
		}
		std::vector<engine::imagegraph::RequestImageSource> imageSources;
		if (!LoadGraphImageInputs(settings, imageSources, failure)) return false;

		const auto evaluate = [&](const std::filesystem::path &destination,
								  uint64_t tick,
								  bool animation,
								  bool bundle = false) {
			std::vector<std::string> values{
				"imagegraph",
				"--input",
				input.string(),
				"--output-id",
				settings.OutputId,
				"--export-scale",
				Number(settings.Scale),
				"--export-filter",
				settings.LinearScaling ? "linear" : "point",
				bundle ? "--bundle" : "--output",
				destination.string()
			};
			if (settings.RetainTemporaryFrames && animation && !bundle && extension == ".apng")
				values.insert(values.end(), {"--debug-frame-directory", stage.Directory.string()});
			if (settings.NativeGif) values.push_back("--require-rgba8");
			if (settings.RigidPlaying) values.push_back("--rigid-playing");
			if (settings.RigidFrameProgress) values.push_back("--rigid-frame-progress");
			if (settings.ArrayIndex)
				values.insert(values.end(), {"--array-index", std::to_string(*settings.ArrayIndex)});
			if (animation) {
				values.insert(
					values.end(),
					{"--frames",
					 std::to_string(settings.Frames.First) + ":" + std::to_string(settings.Frames.Last) +
						 ":" + std::to_string(settings.Frames.Step)}
				);
				if (!bundle) {
					values.insert(values.end(), {"--plays", std::to_string(settings.Plays)});
					if (settings.DelayNumerator && settings.DelayDenominator)
						values.insert(
							values.end(),
							{"--frame-delay",
							 std::to_string(settings.DelayNumerator) + ":" +
								 std::to_string(settings.DelayDenominator)}
						);
					else
						values.insert(
							values.end(), {"--frame-duration-ms", std::to_string(settings.FrameMilliseconds)}
						);
				}
			} else
				values.insert(values.end(), {"--tick", std::to_string(tick)});
			std::vector<char *> pointers;
			for (auto &value : values)
				pointers.push_back(value.data());
			std::ostringstream messages, errors;
			int result = 0;
			if (liveDocument && livePlan && liveRequest) {
				auto request = *liveRequest;
				if (!imageSources.empty()) {
					if (!request.ImageSources.empty()) {
						failure = "Live exports require decoded source grants in the request or ImageInputs, "
								  "not both";
						return false;
					}
					request.ImageSources = imageSources;
				}
				if (!settings.HostCaptures.empty()) request.HostCaptures = settings.HostCaptures;
				if (settings.HostProvider) request.HostProvider = settings.HostProvider;
				result = runner::RunWithDocument(
					static_cast<int>(pointers.size()),
					pointers.data(),
					messages,
					errors,
					*liveDocument,
					*livePlan,
					request
				);
			} else {
				result = runner::RunWithHostInputs(
					static_cast<int>(pointers.size()),
					pointers.data(),
					messages,
					errors,
					imageSources,
					settings.HostCaptures,
					settings.HostProvider
				);
			}
			if (result == 0) return true;
			failure = errors.str();
			return false;
		};
		if (native && !nativeGif) {
			if (!evaluate(target, settings.Frames.First, settings.Animation)) return false;
		} else {
			uint64_t totalBytes = 0;
			const auto bundle = stage.Directory / "frames";
			if (settings.Animation && !evaluate(bundle, settings.Frames.First, true, true)) return false;
			for (size_t index = 0; index < frameCount; index++) {
				const auto frame = FrameName(stage.Directory, index);
				if (settings.Animation) {
					std::ostringstream sourceName;
					sourceName << "frame.tick-" << std::setw(20) << std::setfill('0')
							   << settings.Frames.First + index * settings.Frames.Step << ".png";
					std::filesystem::rename(bundle / sourceName.str(), frame, error);
					if (error) {
						failure = "cannot stage complete evaluated animation frame";
						return false;
					}
				} else if (!evaluate(frame, settings.Frames.First, false))
					return false;
				const uint64_t bytes = std::filesystem::file_size(frame, error);
				if (error || bytes > MAXIMUM_EXPORT_BYTES - totalBytes) {
					failure = "encoder inputs exceed the 512 MiB output budget";
					return false;
				}
				totalBytes += bytes;
			}
			if (settings.Animation) {
				std::filesystem::remove_all(bundle, error);
				if (error) {
					failure = "Cannot discard consumed frame-bundle metadata";
					return false;
				}
			}

			if (nativeGif) {
				constexpr uint64_t memoryBudget = engine::imagegraph::Limits::MaximumEvaluationBytes;
				uint64_t retained =
					frameCount * (sizeof(engine::assets::TextureData) + sizeof(engine::bake::GifFrame));
				std::vector<engine::assets::TextureData> textures;
				std::vector<engine::bake::GifFrame> frames;
				textures.reserve(frameCount);
				frames.reserve(frameCount);
				const double centiseconds = settings.EncoderFramesPerSecond
												? 100 / *settings.EncoderFramesPerSecond
												: settings.FrameMilliseconds / 10.0;
				if (!std::isfinite(centiseconds) || centiseconds < 0 || centiseconds > 65535) {
					failure = "Native GIF centisecond delay exceeds its profile bounds";
					return false;
				}
				const auto delay = static_cast<uint16_t>(std::max(1.0, std::floor(centiseconds)));
				for (size_t index = 0; index < frameCount; index++) {
					const auto file = FrameName(stage.Directory, index);
					const auto size = std::filesystem::file_size(file, error);
					if (error || retained >= memoryBudget || size > memoryBudget - retained) {
						failure = "Native GIF frame input exceeds its working set budget";
						return false;
					}
					std::vector<std::byte> bytes(static_cast<size_t>(size));
					std::ifstream stream(file, std::ios::binary);
					stream.read(
						reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
					);
					if (!stream || !BoundedImageHeader(bytes, (memoryBudget - retained - size) / 2, failure))
						return false;
					engine::assets::TextureData texture;
					if (!engine::bake::ReadImage(bytes, texture, failure) ||
						texture.Format != engine::assets::TextureFormat::RGBA8)
						return false;
					retained += texture.Pixels.size();
					textures.push_back(std::move(texture));
					const auto &stored = textures.back();
					frames.push_back(
						{stored.Width,
						 stored.Height,
						 {reinterpret_cast<const uint8_t *>(stored.Pixels.data()), stored.Pixels.size()},
						 delay}
					);
				}
				std::vector<std::byte> bytes;
				if (!engine::bake::WriteGif(
						frames, settings.NativeGifQuality, memoryBudget - retained, bytes, failure
					))
					return false;
				std::ofstream stream(target, std::ios::binary);
				stream.write(
					reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
				);
				if (!stream) {
					failure = "Cannot stage native GIF output";
					return false;
				}
			} else {
				const auto deadline = std::chrono::steady_clock::now() + settings.EncoderTimeout;
				const auto encode = [&](const std::vector<std::string> &arguments,
										const std::filesystem::path &encodedTarget) {
					engine::parallel::Process process;
					if (!process.Start(encoder, arguments)) {
						failure = "cannot start explicitly granted export encoder";
						return false;
					}
					while (true) {
						const auto status = process.Poll();
						if (!status.Alive()) {
							if (status.Reason != engine::parallel::ExitReason::Exited || status.Code != 0) {
								failure = "export encoder failed";
								return false;
							}
							break;
						}
						const uint64_t produced = std::filesystem::file_size(encodedTarget, error);
						if (!error && produced > MAXIMUM_EXPORT_BYTES) {
							(void)process.Kill();
							(void)process.Wait();
							failure = "encoder output exceeded its byte budget";
							return false;
						}
						error.clear();
						if (std::chrono::steady_clock::now() >= deadline) {
							(void)process.Kill();
							(void)process.Wait();
							failure = "export encoder exceeded its deadline";
							return false;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					}
					const auto bytes = std::filesystem::file_size(encodedTarget, error);
					if (error || bytes == 0 || bytes > MAXIMUM_EXPORT_BYTES - totalBytes) {
						failure = "encoder staged files exceed their byte budget";
						return false;
					}
					totalBytes += bytes;
					return true;
				};
				if (settings.Animation && settings.Output.extension() == ".gif" && settings.GifBatchSize) {
					if (settings.GifBatchSize > 4096) {
						failure = "GIF batch size exceeds its bounds";
						return false;
					}
					std::vector<std::string> mergeArguments;
					for (size_t first = 0; first < frameCount; first += settings.GifBatchSize) {
						const auto count = std::min<size_t>(settings.GifBatchSize, frameCount - first);
						const auto batchDirectory = stage.Directory / ("batch-" + std::to_string(first));
						std::filesystem::create_directory(batchDirectory, error);
						if (error) {
							failure = "cannot stage GIF batch directory";
							return false;
						}
						for (size_t index = 0; index < count; index++) {
							std::filesystem::rename(
								FrameName(stage.Directory, first + index),
								FrameName(batchDirectory, index),
								error
							);
							if (error) {
								failure = "cannot stage GIF batch frame";
								return false;
							}
						}
						const auto batchTarget =
							stage.Directory / ("batch-" + std::to_string(first) + ".gif");
						std::filesystem::path batchEncoder;
						std::vector<std::string> batchArguments;
						if (!BuildGraphEncoderArguments(
								settings,
								batchDirectory,
								batchTarget,
								count,
								batchEncoder,
								batchArguments,
								failure
							) ||
							!encode(batchArguments, batchTarget))
							return false;
						mergeArguments.push_back(batchTarget.string());
					}
					mergeArguments.push_back(target.string());
					if (!encode(mergeArguments, target)) return false;
				} else if (!encode(encoderArguments, target))
					return false;
			}
		}
		const auto status = std::filesystem::symlink_status(target, error);
		if (error || status.type() != std::filesystem::file_type::regular) {
			failure = "encoder did not produce a regular output file";
			return false;
		}
		const uint64_t bytes = std::filesystem::file_size(target, error);
		if (error || bytes == 0 || bytes > MAXIMUM_EXPORT_BYTES) {
			failure = "encoded export violates its byte budget";
			return false;
		}
		if (!native) {
			std::array<unsigned char, 32> header{};
			std::ifstream encoded(target, std::ios::binary);
			encoded.read(reinterpret_cast<char *>(header.data()), header.size());
			const size_t count = static_cast<size_t>(encoded.gcount());
			const auto begins = [&](std::string_view signature, size_t offset = 0) {
				return offset <= count && signature.size() <= count - offset &&
					   std::equal(
						   signature.begin(),
						   signature.end(),
						   header.begin() + static_cast<ptrdiff_t>(offset),
						   [](char expected, unsigned char observed) {
							   return static_cast<unsigned char>(expected) == observed;
						   }
					   );
			};
			const bool valid = ((extension == ".jpg" || extension == ".jpeg") && count >= 3 &&
								header[0] == 255 && header[1] == 216 && header[2] == 255) ||
							   (extension == ".webp" && begins("RIFF") && begins("WEBP", 8)) ||
							   (extension == ".gif" && (begins("GIF89a") || begins("GIF87a"))) ||
							   (extension == ".ico" && count >= 4 && header[0] == 0 && header[1] == 0 &&
								header[2] == 1 && header[3] == 0) ||
							   (extension == ".mp4" && begins("ftyp", 4)) ||
							   (extension == ".webm" && count >= 4 && header[0] == 0x1a &&
								header[1] == 0x45 && header[2] == 0xdf && header[3] == 0xa3) ||
							   (extension == ".txt" && begins("# ImageMagick pixel enumeration:")) ||
							   (extension == ".png" && count >= 8 && header[0] == 137 && begins("PNG", 1));
			if (!valid) {
				failure = "encoder output does not match the requested format signature";
				return false;
			}
		}

		std::filesystem::rename(target, output, error);
		if (error) {
			failure = "cannot publish completed graph export";
			return false;
		}
		stage.Retain = settings.RetainTemporaryFrames && settings.Animation;
		if (retainedTemporaryDirectory)
			*retainedTemporaryDirectory = stage.Retain ? stage.Directory : std::filesystem::path{};
		return true;
	}
	bool
	ExportGraph(const GraphExportSettings &settings, std::string &failure, std::filesystem::path *retained) {
		return ExportGraphImpl(settings, failure, retained, nullptr, nullptr, nullptr);
	}
	bool ExportGraph(
		const GraphExportSettings &settings,
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		std::string &failure,
		std::filesystem::path *retained
	) {
		return ExportGraphImpl(settings, failure, retained, &document, &plan, &request);
	}

	bool ExecuteGraphHostNode(
		const GraphExportSettings &settings,
		std::string_view nodeId,
		engine::imagegraph::HostNodeCapture &capture,
		std::string &failure
	) {
		using namespace engine::imagegraph;
		if (!settings.HostProvider || nodeId.empty() ||
			!settings.Content.AllowsName(settings.Input.string())) {
			failure = "host node execution needs a provider, node ID and allowed graph input";
			return false;
		}
		std::error_code error;
		const auto size = std::filesystem::file_size(settings.Input, error);
		if (error || size > Limits::MaximumDocumentBytes) {
			failure = "host graph input exceeds its document budget";
			return false;
		}
		std::string text(static_cast<size_t>(size), '\0');
		std::ifstream stream(settings.Input, std::ios::binary);
		stream.read(text.data(), static_cast<std::streamsize>(text.size()));
		if (!stream || static_cast<size_t>(stream.gcount()) != text.size()) {
			failure = "cannot read complete host graph document";
			return false;
		}
		Document document;
		Plan plan;
		Diagnostic diagnostic;
		if (Read(text, document, diagnostic) != Status::Ok ||
			Compile(document, plan, diagnostic) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		EvaluationRequest request;
		request.Tick = settings.Frames.First;
		request.RigidPlaying = settings.RigidPlaying;
		request.RigidFrameProgress = settings.RigidFrameProgress;
		request.HostProvider = settings.HostProvider;
		request.HostCaptures = settings.HostCaptures;
		std::vector<RequestImageSource> imageSources;
		if (!LoadGraphImageInputs(settings, imageSources, failure)) return false;
		request.ImageSources = imageSources;
		return ExecuteGraphHostNode(document, plan, request, nodeId, capture, failure);
	}

	bool ExecuteGraphHostNode(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		std::string_view nodeId,
		engine::imagegraph::HostNodeCapture &capture,
		std::string &failure
	) {
		using namespace engine::imagegraph;
		if (!request.HostProvider || nodeId.empty()) {
			failure = "host node execution needs an explicit provider and node ID";
			return false;
		}
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
			return node.Id == nodeId;
		});
		if (node == document.Nodes.end()) {
			failure = "selected host node does not exist";
			return false;
		}
		Diagnostic diagnostic;
		engine::imagegraphphysics::RigidProvider rigidProvider;
		auto evaluationRequest = request;
		if (!evaluationRequest.RigidProvider) evaluationRequest.RigidProvider = &rigidProvider;
		CapturedFeedbackHost replayHost;
		StatefulInputEvaluationResult prepared;
		EvaluationSnapshot directSnapshot;
		const EvaluationSnapshot *resolved = &directSnapshot;
		// Request-only execution continues a caller-owned prior prefix. A completed frame uses the
		// explicit prepared-snapshot overload so constructors and simulation steps are not repeated.
		if (request.SimulationReplay || request.SurfaceReplay || request.RandomReplay || request.DataReplay ||
			request.RigidReplay) {
			if (EvaluateStatefulNodeInputs(document, plan, nodeId, evaluationRequest, prepared, diagnostic) !=
				Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			resolved = &prepared.Inputs;
			evaluationRequest.SimulationReplay = &prepared.Simulation;
			evaluationRequest.SurfaceReplay = &prepared.Surfaces;
			evaluationRequest.RandomReplay = &prepared.Random;
			evaluationRequest.DataReplay = &prepared.Data;
			evaluationRequest.RigidReplay = &prepared.Rigid;
		} else {
			if (!replayHost.PrepareNodeInputs(
					document, plan, request.RigidAuthoringRevision, 0, nodeId, evaluationRequest, diagnostic
				)) {
				failure = diagnostic.Message;
				return false;
			}
			if (replayHost.Active())
				resolved = &replayHost.Snapshot();
			else if (EvaluateNodeInputs(
						 document, plan, nodeId, evaluationRequest, directSnapshot, diagnostic
					 ) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
		}
		return ExecuteGraphHostNode(document, evaluationRequest, nodeId, *resolved, capture, failure);
	}

	bool ExecuteGraphHostNode(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::EvaluationRequest &request,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		engine::imagegraph::HostNodeCapture &capture,
		std::string &failure
	) {
		using namespace engine::imagegraph;
		if (!request.HostProvider || nodeId.empty()) {
			failure = "prepared host execution needs an explicit provider and selected node ID";
			return false;
		}
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end()) {
			failure = "selected prepared host node does not exist";
			return false;
		}
		if (!snapshot.ImageArrays().empty()) {
			failure = "prepared host image arrays need an explicit normalized capture route";
			return false;
		}
		engine::imagegraphphysics::RigidProvider rigidProvider;
		auto evaluationRequest = request;
		if (!evaluationRequest.RigidProvider) evaluationRequest.RigidProvider = &rigidProvider;
		if (snapshot.RetainedBytes() > Limits::MaximumEvaluationBytes / 3) {
			failure = "resolved host inputs exceed their execution budget";
			return false;
		}
		std::vector<AuthoredValue> values;
		std::vector<HostResolvedImage> images;
		values.reserve(snapshot.Values().size());
		images.reserve(snapshot.Images().size());
		for (const auto &value : snapshot.Values())
			values.push_back({value.Port, value.Data});
		for (const auto &image : snapshot.Images())
			images.push_back({image.Port, &image.Data});
		return evaluationRequest.HostProvider->Capture(
			{*node,
			 evaluationRequest,
			 values,
			 images,
			 Limits::MaximumEvaluationBytes - snapshot.RetainedBytes() * 2,
			 document.Timeline ? &*document.Timeline : nullptr,
			 snapshot.InheritedSurfaceFormat(),
			 snapshot.InheritedInterpolation()},
			capture,
			failure
		);
	}

}
