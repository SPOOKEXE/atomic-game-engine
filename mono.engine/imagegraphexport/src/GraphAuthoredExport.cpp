#include "AuthoredExportInternal.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphexport/BuiltinRandomFile.hpp>
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/imagegraphexport/GraphInputs.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		std::filesystem::path ResolvePath(const std::filesystem::path &path, std::error_code &error) {
			const auto absolute = std::filesystem::absolute(path, error);
			return error ? std::filesystem::path{} : std::filesystem::weakly_canonical(absolute, error);
		}
		const Value *Input(const HostNodeInvocation &call, std::string_view port) {
			const auto i = std::find_if(call.Inputs.begin(), call.Inputs.end(), [&](const auto &v) {
				return v.Port == port;
			});
			return i == call.Inputs.end() ? nullptr : &i->Data;
		}
		template <class T> const T *Get(const HostNodeInvocation &call, std::string_view port) {
			const auto *value = Input(call, port);
			return value ? std::get_if<T>(value) : nullptr;
		}
		const std::string *TextAt(const HostNodeInvocation &call, std::string_view port, size_t index) {
			const auto *value = Input(call, port);
			if (!value) return nullptr;
			if (const auto *text = std::get_if<std::string>(value)) return text;
			const auto *array = std::get_if<ArrayValue>(value);
			return array && array->ElementType == ValueType::Text && index < array->Elements.size()
					   ? std::get_if<std::string>(&array->Elements[index])
					   : nullptr;
		}
		bool Number(const HostNodeInvocation &call, std::string_view port, double &number) {
			const auto *value = Input(call, port);
			if (!value) return false;
			if (const auto *v = std::get_if<double>(value))
				number = *v;
			else if (const auto *v = std::get_if<int64_t>(value))
				number = static_cast<double>(*v);
			else if (const auto *v = std::get_if<EnumValue>(value))
				number = static_cast<double>(v->Value);
			else
				return false;
			return std::isfinite(number);
		}
		std::string Text(double value) {
			std::ostringstream text;
			text.imbue(std::locale::classic());
			text << std::setprecision(15) << value;
			return text.str();
		}
		struct Expression {
			std::string_view Text;
			size_t Cursor = 0;
			double Frame = 0, Scale = 1, Index = 0;
			bool Valid = true;
			void Space() {
				while (Cursor < Text.size() && Text[Cursor] == ' ')
					Cursor++;
			}
			double Atom(size_t depth) {
				Space();
				if (depth > 24 || Cursor == Text.size()) {
					Valid = false;
					return 0;
				}
				const char c = Text[Cursor];
				if (c == '+' || c == '-') {
					Cursor++;
					return (c == '-' ? -1 : 1) * Atom(depth + 1);
				}
				if (c == '(') {
					Cursor++;
					const double value = Sum(depth + 1);
					Space();
					if (Cursor == Text.size() || Text[Cursor++] != ')') Valid = false;
					return value;
				}
				if (c == 'f' || c == 'i' || c == 's') {
					Cursor++;
					return c == 'f' ? Frame : c == 's' ? Scale : Index;
				}
				double value = 0;
				const auto parsed = std::from_chars(Text.data() + Cursor, Text.data() + Text.size(), value);
				if (parsed.ec != std::errc{} || parsed.ptr == Text.data() + Cursor) {
					Valid = false;
					return 0;
				}
				Cursor = static_cast<size_t>(parsed.ptr - Text.data());
				return value;
			}
			double Product(size_t depth) {
				double v = Atom(depth);
				while (Valid) {
					Space();
					if (Cursor == Text.size() || (Text[Cursor] != '*' && Text[Cursor] != '/')) break;
					const char op = Text[Cursor++];
					const double b = Atom(depth);
					if (op == '/' && b == 0) {
						Valid = false;
						return 0;
					}
					v = op == '*' ? v * b : v / b;
				}
				return v;
			}
			double Sum(size_t depth) {
				double v = Product(depth);
				while (Valid) {
					Space();
					if (Cursor == Text.size() || (Text[Cursor] != '+' && Text[Cursor] != '-')) break;
					const char op = Text[Cursor++];
					const double b = Product(depth);
					v = op == '+' ? v + b : v - b;
				}
				return v;
			}
		};
		bool Expand(
			std::string_view pattern,
			const std::filesystem::path &directory,
			std::string_view name,
			double frame,
			int64_t begin,
			double scale,
			std::string_view region,
			size_t arrayIndex,
			std::string &result
		) {
			result.clear();
			if (pattern.size() > 4096) return false;
			for (size_t i = 0; i < pattern.size();) {
				if (pattern[i] != '%') {
					result += pattern[i++];
					continue;
				}
				i++;
				unsigned padding = 0;
				while (i < pattern.size() && pattern[i] >= '0' && pattern[i] <= '9') {
					padding = padding * 10 + static_cast<unsigned>(pattern[i++] - '0');
					if (padding > 64) return false;
				}
				if (i == pattern.size()) return false;
				std::string replacement;
				const char token = pattern[i++];
				if (token == '{') {
					const auto end = pattern.find('}', i);
					if (end == std::string_view::npos || end - i > 256) return false;
					Expression expression{
						pattern.substr(i, end - i),
						0,
						static_cast<double>(frame) + 1 + static_cast<double>(begin),
						scale,
						static_cast<double>(arrayIndex)
					};
					const auto value = expression.Sum(0);
					expression.Space();
					if (!expression.Valid || expression.Cursor != expression.Text.size() ||
						!std::isfinite(value))
						return false;
					replacement = Text(value);
					i = end + 1;
				} else if (token == 'd') {
					auto path = directory;
					for (unsigned level = 0; level < padding; level++)
						path = path.parent_path();
					replacement = path.generic_string() + "/";
					padding = 0;
				} else if (token == 'n')
					replacement = name;
				else if (token == 'r')
					replacement = region;
				else if (token == 'f')
					replacement = Text(static_cast<double>(frame) + 1 + static_cast<double>(begin));
				else if (token == 'i')
					replacement = Text(static_cast<double>(arrayIndex));
				else if (token == 's')
					replacement = Text(scale);
				else
					return false;
				if (padding > replacement.size()) result.append(padding - replacement.size(), '0');
				result += replacement;
				if (result.size() > 4096) return false;
			}
			return !result.empty();
		}
	}
	bool PlanAuthoredGraphExport(
		const HostNodeInvocation &call,
		const GraphExportSettings &grants,
		std::span<const GraphExportRegion> regions,
		std::vector<GraphExportSettings> &exports,
		std::string &failure,
		size_t imageCount
	) {
		exports.clear();
		std::vector<GraphExportSettings> planned;
		failure.clear();
		const auto fail = [&](std::string_view message) {
			failure = message;
			return false;
		};
		if (imageCount == 0 || imageCount > 4096 || call.Authored.Type != "pc.export" ||
			grants.Output.empty() || grants.OutputId.empty())
			return fail("authored export needs its preview output and an explicit directory grant");
		const auto *directory = TextAt(call, "directory", 0), *name = TextAt(call, "file_name", 0),
				   *pattern = Get<std::string>(call, "template");
		const auto *custom = Get<bool>(call, "custom_range"), *loop = Get<bool>(call, "loop"),
				   *useRegion = Get<bool>(call, "render_region");
		double type = 0, format = 0, step = 0, begin = 0, quality = 0, bitrate = 0, scale = 0, timing = 0,
			   rate = 0, milliseconds = 0, subformat = 0;
		if (!directory || !name || !pattern || !custom || !loop || !useRegion ||
			!Number(call, "type", type) || !Number(call, "format", format) ||
			!Number(call, "frame_step", step) || !Number(call, "sequence_begin", begin) ||
			!Number(call, "quality_2", quality) || !Number(call, "bit_rate_mbps", bitrate) ||
			!Number(call, "scale", scale) || !Number(call, "frame_timing", timing) ||
			!Number(call, "framerate", rate) || !Number(call, "frame_time_ms", milliseconds) ||
			!Number(call, "subformat", subformat))
			return fail("authored export controls are missing or incorrectly typed");
		if (type < 0 || type > 2 || std::floor(type) != type || format < 0 || std::floor(format) != format ||
			step < 1 || step > 4096 || std::floor(step) != step || std::floor(begin) != begin ||
			std::abs(begin) > 1000000000 || quality < 0 || quality > 100 || bitrate <= 0 || bitrate > 1000 ||
			scale <= 0 || scale > 64 || timing < 0 || timing > 1 || std::floor(timing) != timing ||
			subformat < 0 || subformat > 2 || std::floor(subformat) != subformat)
			return fail("authored export controls exceed their bounded source domains");
		static constexpr std::array still{".png", ".jpg", ".webp", ".exr", ".bmp", ".ico", ".txt"};
		static constexpr std::array animated{".gif", ".apng", ".webp", ".mp4", ".webm"};
		if (format >= static_cast<double>(type == 2 ? animated.size() : still.size()))
			return fail("authored export format index is unknown");
		uint64_t planRowBytes = sizeof(GraphExportSettings) + grants.Input.string().size() +
								grants.Output.string().size() + grants.OutputId.size() +
								grants.ImageEncoder.string().size() + grants.VideoEncoder.string().size() +
								4096;
		if (grants.ImageInputs.size() > Limits::MaximumNodes)
			return fail("authored export image grant count exceeds its budget");
		for (const auto &source : grants.ImageInputs) {
			if (source.SourceId.size() > 255 || source.File.string().size() > 4096)
				return fail("authored export image grant row exceeds its budget");
			planRowBytes += sizeof(GraphImageInput) + source.SourceId.size() + source.File.string().size();
		}
		const uint64_t maximumRows = call.MaximumOperationBytes / 2 / planRowBytes;
		if (maximumRows == 0) return fail("authored export plan exceeds its operation budget");
		auto base = grants;
		base.Animation = type == 2;
		base.Quality = static_cast<uint32_t>(std::round(quality));
		base.EncoderQuality = quality;
		base.MegabitsPerSecond = bitrate;
		base.Plays = *loop ? 0 : 1;
		base.Scale = scale;
		base.PngSubformat = static_cast<uint8_t>(subformat);
		const double fps = call.Timeline ? call.Timeline->FramesPerSecond : 30;
		if (timing == 0) {
			if (rate <= 0 || fps <= 0) return fail("authored export frame rate must be positive");
			double unit = 1;
			if (Input(call, "framerate_unit") &&
				(!Number(call, "framerate_unit", unit) || (unit != 0 && unit != 1)))
				return fail("authored export frame rate unit is invalid");
			milliseconds = std::round(1000 / (rate * (unit == 1 ? fps : 1)));
		}
		if (milliseconds < 1 || milliseconds > 65535)
			return fail("authored export duration exceeds the codec timing domain");
		base.FrameMilliseconds = static_cast<uint16_t>(std::round(milliseconds));
		if (type == 2) {
			const std::string_view extension = animated[static_cast<size_t>(format)];
			if (extension == ".mp4" || extension == ".webm" || extension == ".gif" || extension == ".webp") {
				if (timing == 0) {
					double unit = 1;
					(void)Number(call, "framerate_unit", unit);
					base.EncoderFramesPerSecond = std::max(1.0, rate * (unit == 1 ? fps : 1));
				} else if (extension == ".mp4" || extension == ".webm")
					base.EncoderFramesPerSecond = std::ceil(1000 / milliseconds);
				else
					base.FrameMilliseconds = static_cast<uint16_t>(std::ceil(milliseconds / 10) * 10);
			}
			if (extension == ".apng") {
				double unit = 1;
				(void)Number(call, "framerate_unit", unit);
				const double outputFps = timing == 0 ? std::max(1.0, rate * (unit == 1 ? fps : 1))
													 : std::ceil(1000 / milliseconds);
				if (outputFps > 1000000) return fail("APNG FPS exceeds its bounded domain");
				uint64_t numerator = 1000000,
						 denominator = static_cast<uint64_t>(std::round(outputFps * 1000000));
				const auto divisor = std::gcd(numerator, denominator);
				numerator /= divisor;
				denominator /= divisor;
				if (numerator == 0 || numerator > 65535 || denominator == 0 || denominator > 65535)
					return fail("APNG FPS cannot be represented by its native exact delay fraction");
				base.DelayNumerator = static_cast<uint16_t>(numerator);
				base.DelayDenominator = static_cast<uint16_t>(denominator);
			}
			if (extension == ".gif") {
				double batch = 0;
				if (Input(call, "batch_gif") && (!Number(call, "batch_gif", batch) || batch < 0 ||
												 batch > 4096 || std::floor(batch) != batch))
					return fail("GIF batch size must be an integer from 0 to 4096");
				base.GifBatchSize = static_cast<uint32_t>(batch);
				if (const auto *native = Get<bool>(call, "use_built_in_gif_encoder"))
					base.NativeGif = *native;
				double quantization = 2;
				if (Input(call, "quality") && (!Number(call, "quality", quantization) || quantization < 0 ||
											   quantization > 3 || std::floor(quantization) != quantization))
					return fail("Built-in GIF quantization must be an integer from 0 to 3");
				base.NativeGifQuality = static_cast<uint8_t>(quantization);
				if (const auto *optimise = Get<bool>(call, "frame_optimization"))
					base.OptimiseGif = *optimise;
				double merge = 0.02;
				if (Input(call, "color_merge") && !Number(call, "color_merge", merge))
					return fail("GIF color merge is invalid");
				if (merge < 0 || merge > 1) return fail("GIF color merge exceeds its source domain");
				base.GifColorMerge = merge;
			}
		}
		if (const auto *clear = Get<bool>(call, "attribute_clear_directory"))
			base.RetainTemporaryFrames = !*clear;
		base.Frames = {
			call.Timeline ? call.Timeline->First : 0,
			call.Timeline ? call.Timeline->Last : 0,
			static_cast<uint64_t>(step)
		};
		if (type == 0)
			base.Frames = {call.Request.Tick, call.Request.Tick, 1};
		else if (*custom) {
			const auto *range = Get<Vector2>(call, "frame_range");
			if (!range || range->X < 1 || range->Y < range->X || range->Y > 1e12 ||
				std::floor(range->X) != range->X || std::floor(range->Y) != range->Y)
				return fail("custom export range must contain ordered one-based integral frames");
			base.Frames = {
				static_cast<uint64_t>(range->X - 1),
				static_cast<uint64_t>(range->Y - 1),
				static_cast<uint64_t>(step)
			};
		}
		std::error_code error;
		const auto granted = ResolvePath(grants.Output, error);
		if (error) return fail("cannot resolve export directory grant");
		auto authored = directory->empty() ? granted : ResolvePath(*directory, error);
		if (error || authored.lexically_relative(granted).empty() ||
			authored.lexically_relative(granted).is_absolute() ||
			*authored.lexically_relative(granted).begin() == "..")
			return fail("authored export directory does not equal its explicit grant");

		std::vector<GraphExportRegion> selected;
		if (*useRegion) {
			const auto *names = Get<ArrayValue>(call, "export_regions");
			if (!names || names->ElementType != ValueType::Text || names->Elements.empty() ||
				names->Elements.size() > 4096)
				return fail("region export requires named recorded project regions");
			for (const auto &element : names->Elements) {
				const auto *label = std::get_if<std::string>(&element);
				if (!label) return fail("export region name is not text");
				const auto found = std::find_if(regions.begin(), regions.end(), [&](const auto &r) {
					return r.Name == *label;
				});
				if (found == regions.end())
					return fail("named export region is absent from the recorded project regions");
				selected.push_back(*found);
			}
		} else
			selected.push_back({"", base.Frames});
		for (const auto &region : selected) {
			auto item = base;
			if (*useRegion) item.Frames = region.Frames;
			item.Frames.Step = static_cast<uint64_t>(step);
			size_t count = 0;
			Diagnostic diagnostic;
			if (ValidateTickRange(item.Frames, count, diagnostic) != Status::Ok)
				return fail(diagnostic.Message);
			const size_t files = type == 1 ? count : 1;
			if (files > 4096 - planned.size()) return fail("expanded export count exceeds 4096");
			if (planned.size() > maximumRows || files > (maximumRows - planned.size()) / imageCount)
				return fail("expanded export plan exceeds its working set budget");
			if (files > (4096 - planned.size()) / imageCount)
				return fail("array export count exceeds its bounds");
			for (size_t arrayIndex = 0; arrayIndex < imageCount; arrayIndex++)
				for (size_t index = 0; index < files; index++) {
					auto output = item;
					const auto *memberDirectory = TextAt(call, "directory", arrayIndex),
							   *memberName = TextAt(call, "file_name", arrayIndex);
					if (!memberDirectory || !memberName)
						return fail("array export directory or filename row is absent");
					const auto memberBase = memberDirectory->empty()
												? granted
												: std::filesystem::weakly_canonical(
													  std::filesystem::absolute(*memberDirectory), error
												  );
					const auto relativeBase = memberBase.lexically_relative(granted);
					if (error || relativeBase.empty() || relativeBase.is_absolute() ||
						*relativeBase.begin() == "..")
						return fail("array export directory escapes its exact grant");
					const auto memberStem = memberName->empty()
												? grants.Input.stem().string()
												: std::filesystem::path(*memberName).stem().string();
					if (imageCount > 1) output.ArrayIndex = arrayIndex;
					const auto frame = item.Frames.First + index * item.Frames.Step;
					const double templateFrame =
						type == 0 ? (call.Request.NegativeFrame ? -1 : 1) *
										(static_cast<double>(call.Request.Tick) + call.Request.Subframe)
								  : static_cast<double>(frame);
					if (type == 1) output.Frames = {frame, frame, 1};
					std::string expanded;
					if (!Expand(
							*pattern,
							memberBase,
							memberStem,
							templateFrame,
							static_cast<int64_t>(begin),
							scale,
							region.Name,
							arrayIndex,
							expanded
						))
						return fail("export template contains an unsupported or invalid expression");
					const std::string ext = type == 2 ? animated[static_cast<size_t>(format)]
													  : still[static_cast<size_t>(format)];
					output.Output = std::filesystem::path(expanded + ext);
					if (output.Output.is_relative()) output.Output = granted / output.Output;
					output.Output = std::filesystem::weakly_canonical(output.Output, error);
					if (error) return fail("cannot resolve expanded export target");
					const auto relative = output.Output.lexically_relative(granted);
					if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
						return fail("expanded export path escapes its directory grant");
					if (std::any_of(planned.begin(), planned.end(), [&](const auto &v) {
							return v.Output == output.Output;
						}))
						return fail("export template creates duplicate destination paths");
					planned.push_back(std::move(output));
				}
		}
		exports = std::move(planned);
		return true;
	}
}

namespace engine::imagegraphexport {
	// Each target stages beside its destination so publishing and rollback stay on one filesystem.
	struct BatchExportFile {
		std::filesystem::path Target, Directory, Candidate, Retained, RetainedTarget;
		bool HadPrevious = false, Published = false, RetainedPublished = false;
	};
	bool detail::ExportBatch(
		std::span<GraphExportSettings> exports,
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		bool sequenceFrames,
		std::string &failure,
		std::vector<std::filesystem::path> *retainedDirectories,
		void (*selectTarget)(void *, size_t),
		void *targetContext
	) {
		constexpr uint64_t maximumBatchBytes = 512ull * 1024 * 1024;
		static std::atomic<uint64_t> sequence{0};
		std::vector<BatchExportFile> files;
		std::vector<std::filesystem::path> createdParents;
		files.reserve(exports.size());
		struct Cleanup {
			std::vector<BatchExportFile> &Files;
			std::vector<std::filesystem::path> &CreatedParents;
			bool Keep = false, KeepParents = false;
			~Cleanup() {
				if (Keep) return;
				std::error_code error;
				for (const auto &file : Files)
					std::filesystem::remove_all(file.Directory, error);
				if (!KeepParents)
					for (auto parent = CreatedParents.rbegin(); parent != CreatedParents.rend(); ++parent)
						std::filesystem::remove(*parent, error);
			}
		} cleanup{files, createdParents};
		std::error_code error;
		for (const auto &settings : exports) {
			if (std::any_of(files.begin(), files.end(), [&](const auto &file) {
					return file.Target == settings.Output;
				})) {
				failure = "export batch has duplicate destination paths";
				return false;
			}
			const auto status = std::filesystem::symlink_status(settings.Output, error);
			if (error && status.type() != std::filesystem::file_type::not_found) {
				failure = "cannot inspect export batch destination";
				return false;
			}
			error.clear();
			if (status.type() != std::filesystem::file_type::not_found &&
				status.type() != std::filesystem::file_type::regular) {
				failure = "export batch destination must be absent or a regular file";
				return false;
			}
			std::vector<std::filesystem::path> missingParents;
			for (auto parent = settings.Output.parent_path(); !parent.empty();) {
				if (std::filesystem::exists(parent, error)) {
					if (error || !std::filesystem::is_directory(parent, error) || error) {
						failure = "export batch parent is not a directory";
						return false;
					}
					break;
				}
				if (error || !settings.Content.AllowsName(parent.string()) || missingParents.size() >= 4096) {
					failure = "export batch parent directory is refused or unbounded";
					return false;
				}
				missingParents.push_back(parent);
				const auto next = parent.parent_path();
				if (next == parent) break;
				parent = next;
			}
			for (auto parent = missingParents.rbegin(); parent != missingParents.rend(); ++parent) {
				createdParents.push_back(*parent);
				if (!std::filesystem::create_directory(*parent, error) || error) {
					createdParents.pop_back();
					failure = "cannot create a granted export batch parent directory";
					return false;
				}
			}
			const auto directory =
				settings.Output.parent_path() /
				(".graph-export-batch-" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
				 std::to_string(sequence.fetch_add(1)));
			BatchExportFile file{
				settings.Output,
				directory,
				directory / settings.Output.filename(),
				{},
				{},
				false,
				false,
				false
			};
			if (!settings.Content.AllowsName(settings.Output.string()) ||
				!settings.Content.AllowsName(directory.string()) ||
				!std::filesystem::create_directory(directory, error) || error) {
				failure = "cannot create an exclusive granted export batch directory";
				return false;
			}
			files.push_back(std::move(file));
		}
		uint64_t stagedBytes = 0;
		for (size_t index = 0; index < exports.size(); ++index) {
			auto settings = exports[index];
			settings.Output = files[index].Candidate;
			auto frameRequest = request;
			if (sequenceFrames) {
				frameRequest.Tick = settings.Frames.First;
				frameRequest.Subframe = 0;
				frameRequest.NegativeFrame = false;
			}
			if (selectTarget) selectTarget(targetContext, index);
			if (!ExportGraph(settings, document, plan, frameRequest, failure, &files[index].Retained))
				return false;
			std::filesystem::recursive_directory_iterator entries(files[index].Directory, error), end;
			if (error) {
				failure = "cannot inspect staged export batch";
				return false;
			}
			for (; entries != end; entries.increment(error)) {
				if (error) {
					failure = "cannot inspect staged export batch";
					return false;
				}
				const auto status = entries->symlink_status(error);
				if (error || (status.type() != std::filesystem::file_type::directory &&
							  status.type() != std::filesystem::file_type::regular)) {
					failure = "export batch staging contains an unexpected file type";
					return false;
				}
				if (status.type() == std::filesystem::file_type::regular) {
					const auto bytes = entries->file_size(error);
					if (error || bytes > maximumBatchBytes - stagedBytes) {
						failure = "export batch exceeds its aggregate staged byte budget";
						return false;
					}
					stagedBytes += bytes;
				}
			}
			if (error) {
				failure = "cannot inspect staged export batch";
				return false;
			}
		}
		std::vector<std::filesystem::path> retained;
		if (retainedDirectories) retained = *retainedDirectories;
		for (auto &file : files) {
			if (!file.Retained.empty()) {
				file.RetainedTarget = file.Target.parent_path() / file.Retained.filename();
				if (retainedDirectories) retained.push_back(file.RetainedTarget);
			}
		}
		const auto rollback = [&] {
			bool restored = true;
			for (auto file = files.rbegin(); file != files.rend(); ++file) {
				if (file->RetainedPublished) {
					std::filesystem::remove_all(file->RetainedTarget, error);
					if (error) restored = false;
				}
				if (file->Published) {
					std::filesystem::remove(file->Target, error);
					if (error) restored = false;
				}
				if (file->HadPrevious) {
					error.clear();
					std::filesystem::rename(file->Directory / "previous", file->Target, error);
					if (error) restored = false;
				}
			}
			if (!restored) {
				cleanup.Keep = true;
				failure += "; rollback failed; prior files remain in export batch backup directories";
			}
			return false;
		};
		for (auto &file : files) {
			const auto status = std::filesystem::symlink_status(file.Target, error);
			if ((error && status.type() != std::filesystem::file_type::not_found) ||
				(status.type() != std::filesystem::file_type::not_found &&
				 status.type() != std::filesystem::file_type::regular)) {
				failure = "export batch destination changed before publication";
				return rollback();
			}
			error.clear();
			if (status.type() == std::filesystem::file_type::regular) {
				std::filesystem::rename(file.Target, file.Directory / "previous", error);
				if (error) {
					failure = "cannot back up export batch destination";
					return rollback();
				}
				file.HadPrevious = true;
			}
			std::filesystem::rename(file.Candidate, file.Target, error);
			if (error) {
				failure = "cannot publish complete export batch";
				return rollback();
			}
			file.Published = true;
		}
		for (auto &file : files) {
			if (file.Retained.empty()) continue;
			const auto &target = file.RetainedTarget;
			if (!exports.front().Content.AllowsName(target.string()) ||
				std::filesystem::exists(target, error) || error) {
				failure = "cannot publish retained export batch frames";
				return rollback();
			}
			std::filesystem::rename(file.Retained, target, error);
			if (error) {
				failure = "cannot publish retained export batch frames";
				return rollback();
			}
			file.RetainedPublished = true;
		}
		if (retainedDirectories) retainedDirectories->swap(retained);
		cleanup.KeepParents = true;
		return true;
	}
	bool detail::PlanPreparedAuthoredExport(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Node &node,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		const engine::imagegraph::EvaluationRequest &request,
		const GraphExportSettings &grants,
		std::span<const GraphExportRegion> regions,
		std::vector<GraphExportSettings> &exports,
		size_t &imageCount,
		double &exportType,
		std::string &failure
	) {
		using namespace engine::imagegraph;

		if (snapshot.RetainedBytes() > Limits::MaximumEvaluationBytes / 3) {
			failure = "authored prepared inputs exceed their export budget";
			return false;
		}
		std::vector<AuthoredValue> values;
		for (const auto &value : snapshot.Values())
			values.push_back({value.Port, value.Data});
		std::vector<HostResolvedImage> images;
		for (const auto &image : snapshot.Images())
			images.push_back({image.Port, &image.Data});
		imageCount = 1;
		bool arraySurface = false;
		for (const auto &array : snapshot.ImageArrays())
			if (array.Port == "surface") {
				arraySurface = true;
				imageCount = array.Data.Items.size();
				for (const auto &item : array.Data.Items)
					if (!std::holds_alternative<size_t>(item.Data)) {
						failure = "authored export surface array must have native flat image members";
						return false;
					}
			}
		HostNodeInvocation invocation{
			node,
			request,
			values,
			images,
			Limits::MaximumEvaluationBytes,
			document.Timeline ? &*document.Timeline : nullptr,
			snapshot.InheritedSurfaceFormat()
		};
		std::vector<GraphExportRegion> storedRegions;
		if (regions.empty() && document.Project) {
			const auto *selected = Get<ArrayValue>(invocation, "export_regions");
			const auto *enabled = Get<bool>(invocation, "render_region");
			if (enabled && *enabled && selected) {
				for (const auto &item : selected->Elements) {
					const auto *label = std::get_if<std::string>(&item);
					if (!label) continue;
					const auto found = std::find_if(
						document.Project->AnimationRegions.begin(),
						document.Project->AnimationRegions.end(),
						[&](const auto &region) { return region.Label == *label; }
					);
					if (found == document.Project->AnimationRegions.end()) continue;
					if (found->Start.NegativeFrame || found->End.NegativeFrame ||
						found->Start.Subframe != 0 || found->End.Subframe != 0) {
						failure =
							"Export frame range requires integral nonnegative animation region endpoints";
						return false;
					}
					storedRegions.push_back({found->Label, {found->Start.Tick, found->End.Tick, 1}});
				}
			}
			regions = storedRegions;
		}
		if (!PlanAuthoredGraphExport(invocation, grants, regions, exports, failure, imageCount)) return false;
		for (auto &settings : exports) {
			if (arraySurface && !settings.ArrayIndex) settings.ArrayIndex = 0;
			settings.LinearScaling = snapshot.InheritedInterpolation() == 1;
		}
		exportType = 0;
		(void)Number(invocation, "type", exportType);
		return true;
	}

	static bool ExportAuthoredGraphImpl(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		const GraphExportSettings &grants,
		std::string_view nodeId,
		std::span<const GraphExportRegion> regions,
		std::string &failure,
		std::vector<std::filesystem::path> *retainedTemporaryDirectories,
		const engine::imagegraph::EvaluationSnapshot *preparedInputs = nullptr
	) {
		ENGINE_PROFILE_CAT("image composer authored export", engine::core::ProfileCategory::Engine);
		using namespace engine::imagegraph;
		Diagnostic diagnostic;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &v) {
			return v.Id == nodeId && v.Type == "pc.export";
		});
		if (node == document.Nodes.end()) {
			failure = "authored export node is absent";
			return false;
		}
		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &v) {
				return v.Id == grants.OutputId && v.NodeId == nodeId && v.Port == "preview";
			});
		if (output == document.Outputs.end()) {
			failure = "selected output must bind this export node preview";
			return false;
		}
		EvaluationSnapshot directInputs;
		CapturedFeedbackHost inputHost;
		engine::imagegraphphysics::RigidProvider rigidProvider;
		auto inputRequest = request;
		if (!inputRequest.RigidProvider) inputRequest.RigidProvider = &rigidProvider;
		if (!preparedInputs) {
			if (!inputHost.PrepareNodeInputs(
					document, plan, request.RigidAuthoringRevision, 0, nodeId, inputRequest, diagnostic
				)) {
				failure = diagnostic.Message;
				return false;
			}
			if (inputHost.Active())
				preparedInputs = &inputHost.Snapshot();
			else {
				if (EvaluateNodeInputs(document, plan, nodeId, inputRequest, directInputs, diagnostic) !=
					Status::Ok) {
					failure = diagnostic.Message;
					return false;
				}
				preparedInputs = &directInputs;
			}
		}
		const auto &snapshot = *preparedInputs;
		std::vector<GraphExportSettings> exports;
		size_t imageCount = 1;
		double exportType = 0;
		if (!detail::PlanPreparedAuthoredExport(
				document, *node, snapshot, request, grants, regions, exports, imageCount, exportType, failure
			))
			return false;
		if (exportType == 0) {
			if (imageCount > std::min(Limits::MaximumNodes, Limits::MaximumOutputs)) {
				failure = "prepared export surface count exceeds its captured graph bound";
				return false;
			}
			std::vector<const Image *> surfaces;
			for (const auto &image : snapshot.Images())
				if (image.Port == "surface") surfaces.push_back(&image.Data);
			for (const auto &array : snapshot.ImageArrays()) {
				if (array.Port != "surface") continue;
				for (const auto &item : array.Data.Items) {
					const auto *index = std::get_if<size_t>(&item.Data);
					if (!index || *index >= array.Data.Images.size()) {
						failure = "prepared export surface array has an invalid image reference";
						return false;
					}
					surfaces.push_back(&array.Data.Images[*index]);
				}
			}
			if (surfaces.empty() ||
				surfaces.size() > std::min(Limits::MaximumNodes, Limits::MaximumOutputs)) {
				failure = "prepared export surface count exceeds its captured graph bound";
				return false;
			}
			Document capturedDocument;
			std::vector<RequestImageSource> captures;
			uint64_t retained = snapshot.RetainedBytes();
			for (size_t index = 0; index < surfaces.size(); ++index) {
				const auto bytes = surfaces[index]->Pixels.capacity() + sizeof(RequestImageSource) +
								   sizeof(Node) + sizeof(Output) + 1024;
				if (bytes > Limits::MaximumEvaluationBytes / 2 -
								std::min(retained, Limits::MaximumEvaluationBytes / 2)) {
					failure = "prepared export captured surfaces exceed their combined byte budget";
					return false;
				}
				retained += bytes;
				const auto id = "prepared/" + std::to_string(index);
				capturedDocument.Nodes.push_back({id, "image.captured", "", {}, {{"source_id", id}}});
				capturedDocument.Outputs.push_back({id, id, "image"});
				captures.push_back({id, *surfaces[index]});
				engine::core::Metrics::Count(
					"image composer prepared image clone bytes",
					static_cast<double>(captures.back().Data.Pixels.size())
				);
				engine::core::Metrics::Count("image composer prepared image clones", 1);
			}
			Plan capturedPlan;
			if (Compile(capturedDocument, capturedPlan, diagnostic) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			for (auto &settings : exports) {
				const auto index = settings.ArrayIndex.value_or(0);
				if (index >= surfaces.size()) {
					failure = "prepared export selects an absent surface member";
					return false;
				}
				settings.OutputId = "prepared/" + std::to_string(index);
				settings.ArrayIndex.reset();
				settings.ImageInputs.clear();
			}
			EvaluationRequest capturedRequest;
			(void)SetFrameTime(capturedRequest, {request.Tick, request.Subframe, request.NegativeFrame});
			capturedRequest.MaximumImageDimension = request.MaximumImageDimension;
			capturedRequest.ImageSources = captures;
			return detail::ExportBatch(
				exports,
				capturedDocument,
				capturedPlan,
				capturedRequest,
				false,
				failure,
				retainedTemporaryDirectories
			);
		}
		// A completed current-frame journal is not the prefix of the first requested animation frame.
		// The shared runner reconstructs the complete range with one fresh replay owner.
		auto rangeRequest = request;
		rangeRequest.SimulationReplay = nullptr;
		rangeRequest.SurfaceReplay = nullptr;
		rangeRequest.RandomReplay = nullptr;
		rangeRequest.DataReplay = nullptr;
		rangeRequest.RigidReplay = nullptr;
		rangeRequest.ReuseSimulationFrame = false;
		return detail::ExportBatch(
			exports, document, plan, rangeRequest, true, failure, retainedTemporaryDirectories
		);
	}

	bool ExportAuthoredGraphNode(
		const GraphExportSettings &grants,
		std::string_view nodeId,
		std::span<const GraphExportRegion> regions,
		std::string &failure,
		std::vector<std::filesystem::path> *retainedTemporaryDirectories
	) {
		using namespace engine::imagegraph;
		std::error_code error;
		const auto size = std::filesystem::file_size(grants.Input, error);
		if (error || size > Limits::MaximumDocumentBytes ||
			!grants.Content.AllowsName(grants.Input.string())) {
			failure = "authored export document is refused or unbounded";
			return false;
		}
		std::string text(static_cast<size_t>(size), '\0');
		std::ifstream stream(grants.Input, std::ios::binary);
		stream.read(text.data(), static_cast<std::streamsize>(text.size()));
		if (!stream) {
			failure = "cannot read complete authored export graph";
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
		std::vector<RequestImageSource> sources;
		if (!LoadGraphImageInputs(grants, sources, failure)) return false;
		EvaluationRequest request;
		std::vector<SourceBuiltinRandomCapture> builtinRandomCaptures;
		if (!grants.BuiltinRandomCapture.empty()) {
			if (!LoadBuiltinRandomCaptureFile(
					grants.BuiltinRandomCapture, grants.Content, builtinRandomCaptures, failure
				))
				return false;
			request.BuiltinRandomCaptures = builtinRandomCaptures;
		}
		request.Tick = grants.Frames.First;
		request.RigidPlaying = grants.RigidPlaying;
		request.RigidFrameProgress = grants.RigidFrameProgress;
		request.HostProvider = grants.HostProvider;
		request.HostCaptures = grants.HostCaptures;
		request.ImageSources = sources;
		auto liveGrants = grants;
		liveGrants.ImageInputs.clear();
		return ExportAuthoredGraphImpl(
			document, plan, request, liveGrants, nodeId, regions, failure, retainedTemporaryDirectories
		);
	}

	static bool ExportAuthoredGraphLive(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		const GraphExportSettings &grants,
		std::string_view nodeId,
		std::string &failure,
		std::vector<std::filesystem::path> *retainedTemporaryDirectories,
		const engine::imagegraph::EvaluationSnapshot *preparedInputs
	) {
		using namespace engine::imagegraph;
		auto resolvedGrants = grants;
		if (resolvedGrants.OutputId.empty()) {
			const auto found =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &output) {
					return output.NodeId == nodeId && output.Port == "preview";
				});
			if (found != document.Outputs.end())
				resolvedGrants.OutputId = found->Id;
			else {
				const auto bytes = DocumentRetainedPayloadBytes(document);
				if (!bytes || *bytes > Limits::MaximumEvaluationBytes / 4 ||
					document.Outputs.size() >= Limits::MaximumOutputs) {
					failure = "Ephemeral export preview exceeds document copy or output bounds";
					return false;
				}
				auto copy = document;
				for (size_t index = 0;; index++) {
					resolvedGrants.OutputId = "__export_preview_" + std::to_string(index);
					if (std::none_of(copy.Outputs.begin(), copy.Outputs.end(), [&](const auto &output) {
							return output.Id == resolvedGrants.OutputId;
						}))
						break;
				}
				copy.Outputs.push_back({resolvedGrants.OutputId, std::string(nodeId), "preview"});
				Plan copiedPlan;
				Diagnostic diagnostic;
				if (Compile(copy, copiedPlan, diagnostic) != Status::Ok) {
					failure = diagnostic.Message;
					return false;
				}
				return ExportAuthoredGraphImpl(
					copy,
					copiedPlan,
					request,
					resolvedGrants,
					nodeId,
					{},
					failure,
					retainedTemporaryDirectories,
					preparedInputs
				);
			}
		}
		return ExportAuthoredGraphImpl(
			document,
			plan,
			request,
			resolvedGrants,
			nodeId,
			{},
			failure,
			retainedTemporaryDirectories,
			preparedInputs
		);
	}

	bool ExportAuthoredGraphNode(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		const GraphExportSettings &grants,
		std::string_view nodeId,
		std::string &failure,
		std::vector<std::filesystem::path> *retained
	) {
		return ExportAuthoredGraphLive(document, plan, request, grants, nodeId, failure, retained, nullptr);
	}
	bool ExportAuthoredGraphNode(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request,
		const GraphExportSettings &grants,
		std::string_view nodeId,
		const engine::imagegraph::EvaluationSnapshot &preparedInputs,
		std::string &failure,
		std::vector<std::filesystem::path> *retained
	) {
		return ExportAuthoredGraphLive(
			document, plan, request, grants, nodeId, failure, retained, &preparedInputs
		);
	}

}
