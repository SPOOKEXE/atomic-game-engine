#include <engine/imagegraphexport/GraphCommandHost.hpp>
#include <engine/imagegraphexport/GraphDeviceHost.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphexport/GraphInputs.hpp>
#include <engine/parallel/Process.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &call, std::string_view port) {
			const auto i = std::find_if(call.Inputs.begin(), call.Inputs.end(), [&](const auto &v) {
				return v.Port == port;
			});
			return i == call.Inputs.end() ? nullptr : &i->Data;
		}
		template <class T> const T *Get(const HostNodeInvocation &call, std::string_view port) {
			const auto *v = Input(call, port);
			return v ? std::get_if<T>(v) : nullptr;
		}
		bool ValidExecutable(const std::filesystem::path &path) {
			std::error_code error;
			return path.is_absolute() && std::filesystem::is_regular_file(path, error) && !error;
		}
		bool Deadline(std::chrono::milliseconds timeout) {
			return timeout.count() > 0 && timeout <= std::chrono::minutes(10);
		}
		HostNodeCapture Bind(const HostNodeInvocation &call) {
			HostNodeCapture capture;
			capture.Authored = call.Authored;
			capture.Tick = call.Request.Tick;
			capture.Subframe = call.Request.Subframe;
			capture.NegativeFrame = call.Request.NegativeFrame;
			capture.Inputs.assign(call.Inputs.begin(), call.Inputs.end());
			for (const auto &image : call.Images)
				if (image.Data)
					capture.InputImages.push_back({std::string(image.Port), SurfaceHash(*image.Data)});
			return capture;
		}
		bool Wait(
			engine::parallel::Process &process,
			std::chrono::milliseconds timeout,
			const std::filesystem::path &file,
			uint64_t maximum,
			std::string &failure
		) {
			const auto until = std::chrono::steady_clock::now() + timeout;
			while (true) {
				const auto state = process.Poll();
				std::error_code error;
				if (!file.empty() && std::filesystem::exists(file, error)) {
					const auto size = std::filesystem::file_size(file, error);
					if (error || size > maximum) {
						(void)process.Kill();
						(void)process.Wait();
						failure = "host response exceeds its explicit byte budget";
						return false;
					}
				}
				if (!state.Alive()) {
					if (state.Reason != engine::parallel::ExitReason::Exited || state.Code != 0) {
						failure = "explicit host command failed";
						return false;
					}
					return true;
				}
				if (std::chrono::steady_clock::now() >= until) {
					(void)process.Kill();
					(void)process.Wait();
					failure = "explicit host command exceeded its deadline";
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
		bool Process(
			std::span<const GraphProcessGrant> grants,
			const HostNodeInvocation &call,
			HostNodeCapture &out,
			std::string &failure
		) {
			const GraphProcessGrant *grant = nullptr;
			for (const auto &g : grants)
				if (g.NodeId == call.Authored.Id) {
					if (grant) {
						failure = "process grants are duplicated";
						return false;
					}
					grant = &g;
				}
			const auto *path = Get<std::string>(call, "path"), *script = Get<std::string>(call, "script");
			std::error_code error;
			if (!grant || !path || !script || *path != grant->Executable.string() ||
				*script != grant->Script || !ValidExecutable(grant->Executable) ||
				!Deadline(grant->Timeout) || !grant->WorkingDirectory.is_absolute() ||
				!std::filesystem::is_directory(grant->WorkingDirectory, error) || error ||
				grant->Arguments.size() > 256) {
				failure =
					"process needs exact path/script, literal argv, absolute cwd and bounded deadline grants";
				return false;
			}
			size_t bytes = 0;
			for (const auto &argument : grant->Arguments) {
				if (argument.find('\0') != std::string::npos || argument.size() > 65536 - bytes) {
					failure = "process literal argv exceeds its bounds";
					return false;
				}
				bytes += argument.size();
			}
			if (bytes > call.MaximumOperationBytes) {
				failure = "process argv exceeds its operation budget";
				return false;
			}
			engine::parallel::Process process;
			if (!process.Start(grant->Executable, grant->Arguments, grant->WorkingDirectory)) {
				failure = "cannot start explicitly granted executable";
				return false;
			}
			if (!Wait(process, grant->Timeout, {}, 0, failure)) return false;
			out = Bind(call);
			return true;
		}
		bool Http(
			std::span<const GraphHttpGrant> grants,
			const engine::assets::ContentPolicy &policy,
			const HostNodeInvocation &call,
			HostNodeCapture &out,
			std::string &failure
		) {
			const GraphHttpGrant *grant = nullptr;
			for (const auto &g : grants)
				if (g.NodeId == call.Authored.Id) {
					if (grant) {
						failure = "HTTP grants are duplicated";
						return false;
					}
					grant = &g;
				}
			const auto *address = Get<std::string>(call, "address");
			const bool image = call.Authored.Type == "pc.http_request_file";
			const auto *type = Get<EnumValue>(call, image ? "format" : "type");
			const auto *content = Get<std::string>(call, "content");
			if (!grant || !address || !type || *address != grant->Address ||
				!ValidExecutable(grant->Client) || !Deadline(grant->Timeout) || grant->MaximumBytes == 0 ||
				grant->MaximumBytes > 16 * 1024 * 1024 ||
				grant->MaximumBytes > call.MaximumOperationBytes / 4 || address->size() > 4096 ||
				!(address->starts_with("http://") || address->starts_with("https://")) ||
				address->find_first_of("\r\n\t") != std::string::npos ||
				address->find('\0') != std::string::npos) {
				failure = "HTTP needs an exact bounded URL and explicit client grant";
				return false;
			}
			const auto authority = std::string_view(*address).substr(address->find("://") + 3);
			const auto host = authority.substr(0, authority.find('/'));
			if (host.empty() || host.find('@') != std::string_view::npos) {
				failure = "HTTP grant URL authority is invalid";
				return false;
			}
			if (image ? (grant->Post || type->Value != 0)
					  : (type->Value != (grant->Post ? 1 : 0) || !content || *content != grant->Content)) {
				failure = "HTTP method, content or source image format differs from its grant";
				return false;
			}
			if (grant->Content.size() > grant->MaximumBytes ||
				grant->Content.find('\0') != std::string::npos) {
				failure = "HTTP post content exceeds its grant";
				return false;
			}
			uint64_t maximum = grant->MaximumBytes;
			if (const auto *limit = Get<double>(call, "attribute_max_file_size")) {
				if (!std::isfinite(*limit) || *limit < 1 || *limit > 16 * 1024 * 1024) {
					failure = "HTTP node byte limit is invalid";
					return false;
				}
				maximum = std::min(maximum, static_cast<uint64_t>(*limit));
			}
			static std::atomic<uint64_t> sequence{0};
			std::error_code error;
			const auto directory =
				std::filesystem::temp_directory_path() /
				("atomic-http-" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
				 std::to_string(sequence.fetch_add(1)));
			if (!std::filesystem::create_directory(directory, error) || error) {
				failure = "cannot create exclusive HTTP staging directory";
				return false;
			}
			struct Cleanup {
				std::filesystem::path Path;
				~Cleanup() {
					std::error_code error;
					std::filesystem::remove_all(Path, error);
				}
			} cleanup{directory};
			const auto response = directory / "response.bin";
			std::vector<std::string> args{
				"--disable",	  "--silent",
				"--show-error",	  "--fail",
				"--noproxy",	  "*",
				"--proxy",		  "",
				"--proto",		  "=http,https",
				"--max-redirs",	  "0",
				"--max-time",	  std::to_string(static_cast<double>(grant->Timeout.count()) / 1000),
				"--max-filesize", std::to_string(maximum),
				"--output",		  response.string(),
				"--request",	  grant->Post ? "POST" : "GET"
			};
			if (grant->Post) {
				const auto post = directory / "post.txt";
				std::ofstream stream(post, std::ios::binary);
				stream.write(grant->Content.data(), static_cast<std::streamsize>(grant->Content.size()));
				stream.close();
				if (!stream) {
					failure = "cannot stage complete HTTP request body";
					return false;
				}
				args.insert(
					args.end(),
					{"--header",
					 "Content-Type: application/x-www-form-urlencoded",
					 "--data-binary",
					 "@" + post.string()}
				);
			}
			args.insert(args.end(), {"--url", *address});
			engine::parallel::Process process;
			if (!process.Start(grant->Client, args, directory)) {
				failure = "cannot start explicit HTTP client";
				return false;
			}
			if (!Wait(process, grant->Timeout, response, maximum, failure)) return false;
			const auto size = std::filesystem::file_size(response, error);
			if (error || size > maximum) {
				failure = "HTTP response is missing or exceeds its budget";
				return false;
			}
			auto capture = Bind(call);
			if (image) {
				GraphExportSettings settings;
				settings.ImageInputs.push_back({"http", response});
				settings.Content = policy;
				std::vector<RequestImageSource> sources;
				if (!LoadGraphImageInputs(settings, sources, failure, call.MaximumOperationBytes / 2))
					return false;
				if (sources.size() != 1 || sources[0].Data.Pixels.size() > call.MaximumOperationBytes / 2) {
					failure = "decoded HTTP image exceeds its operation budget";
					return false;
				}
				capture.Images.push_back({"result", std::move(sources[0].Data)});
			} else {
				std::string body(static_cast<size_t>(size), '\0');
				std::ifstream stream(response, std::ios::binary);
				stream.read(body.data(), static_cast<std::streamsize>(body.size()));
				if (!stream) {
					failure = "cannot read complete HTTP response";
					return false;
				}
				capture.Outputs.push_back({"result", std::move(body)});
			}
			out = std::move(capture);
			return true;
		}
	}
	bool ReadGraphCommandGrants(
		std::string_view text,
		std::vector<GraphProcessGrant> &processes,
		std::vector<GraphHttpGrant> &requests,
		std::string &failure,
		std::vector<std::string> *clockNodes
	) {
		if (text.size() > 1048576) {
			failure = "command grant document exceeds one MiB";
			return false;
		}
		std::istringstream stream{std::string(text)};
		std::string magic;
		unsigned version = 0;
		if (!(stream >> magic >> version) || magic != "imagegraph-host-grants" || version != 1) {
			failure = "unsupported command grant document version";
			return false;
		}
		std::vector<GraphProcessGrant> parsedProcesses;
		std::vector<GraphHttpGrant> parsedRequests;
		std::vector<std::string> parsedClocks;
		std::string kind;
		while (stream >> kind) {
			if (parsedProcesses.size() + parsedRequests.size() + parsedClocks.size() >= 4096) {
				failure = "command grant count exceeds 4096";
				return false;
			}
			uint64_t timeout = 0;
			if (kind == "clock") {
				std::string node;
				if (!clockNodes || !(stream >> std::quoted(node)) || node.empty() || node.size() > 255 ||
					std::find(parsedClocks.begin(), parsedClocks.end(), node) != parsedClocks.end()) {
					failure = "malformed or duplicate clock grant";
					return false;
				}
				parsedClocks.push_back(std::move(node));
			} else if (kind == "process") {
				GraphProcessGrant grant;
				std::string path, cwd;
				size_t count = 0;
				if (!(stream >> std::quoted(grant.NodeId) >> std::quoted(path) >> std::quoted(cwd) >>
					  std::quoted(grant.Script) >> timeout >> count) ||
					count > 256) {
					failure = "malformed process grant";
					return false;
				}
				grant.Executable = path;
				grant.WorkingDirectory = cwd;
				if (timeout == 0 || timeout > 600000) {
					failure = "command grant timeout exceeds its bounds";
					return false;
				}
				grant.Timeout = std::chrono::milliseconds(timeout);
				for (size_t i = 0; i < count; i++) {
					std::string argument;
					if (!(stream >> std::quoted(argument))) {
						failure = "truncated process grant argv";
						return false;
					}
					grant.Arguments.push_back(std::move(argument));
				}
				if (grant.NodeId.empty() || grant.NodeId.size() > 255 || !Deadline(grant.Timeout) ||
					!grant.Executable.is_absolute() || !grant.WorkingDirectory.is_absolute()) {
					failure = "process grant domains are invalid";
					return false;
				}
				parsedProcesses.push_back(std::move(grant));
			} else if (kind == "http") {
				GraphHttpGrant grant;
				unsigned post = 0;
				std::string client;
				if (!(stream >> std::quoted(grant.NodeId) >> std::quoted(grant.Address) >> post >>
					  std::quoted(grant.Content) >> std::quoted(client) >> grant.MaximumBytes >> timeout) ||
					post > 1) {
					failure = "malformed HTTP grant";
					return false;
				}
				grant.Post = post == 1;
				grant.Client = client;
				if (timeout == 0 || timeout > 600000) {
					failure = "command grant timeout exceeds its bounds";
					return false;
				}
				grant.Timeout = std::chrono::milliseconds(timeout);
				if (grant.NodeId.empty() || grant.NodeId.size() > 255 || grant.Address.size() > 4096 ||
					grant.MaximumBytes == 0 || grant.MaximumBytes > 16 * 1024 * 1024 ||
					!Deadline(grant.Timeout) || !grant.Client.is_absolute()) {
					failure = "HTTP grant domains are invalid";
					return false;
				}
				parsedRequests.push_back(std::move(grant));
			} else {
				failure = "unknown command grant kind";
				return false;
			}
		}
		if (!stream.eof()) {
			failure = "malformed command grant document";
			return false;
		}
		processes = std::move(parsedProcesses);
		requests = std::move(parsedRequests);
		if (clockNodes) *clockNodes = std::move(parsedClocks);
		return true;
	}
	GraphCommandHost::GraphCommandHost(
		std::span<const GraphProcessGrant> processes,
		std::span<const GraphHttpGrant> requests,
		engine::assets::ContentPolicy policy,
		std::span<const std::string> clockNodes
	)
		: Processes(processes), Requests(requests), Policy(policy), ClockNodes(clockNodes) {}
	bool GraphCommandHost::Capture(
		const engine::imagegraph::HostNodeInvocation &call,
		engine::imagegraph::HostNodeCapture &out,
		std::string &failure
	) {
		failure.clear();
		if (call.Authored.Type == "pc.datetime_get") {
			if (ClockNodes.size() > 4096 ||
				std::count(ClockNodes.begin(), ClockNodes.end(), call.Authored.Id) != 1) {
				failure = "DateTime requires an explicit unique clock grant";
				return false;
			}
			std::tm calendar{};
			const auto now = std::time(nullptr);
			// localtime exposes shared standard-library storage; copy it under the adapter lock.
			static std::mutex calendarMutex;
			{
				std::lock_guard lock(calendarMutex);
				const auto *observed = std::localtime(&now);
				if (!observed) {
					failure = "Host local calendar observation failed";
					return false;
				}
				calendar = *observed;
			}
			GraphDateTimeFrame frame;
			frame.NodeId = call.Authored.Id;
			frame.Tick = call.Request.Tick;
			frame.Subframe = call.Request.Subframe;
			frame.NegativeFrame = call.Request.NegativeFrame;
			frame.Year = calendar.tm_year + 1900;
			frame.Month = calendar.tm_mon + 1;
			frame.Day = calendar.tm_mday;
			frame.Weekday = calendar.tm_wday;
			frame.Hour = calendar.tm_hour;
			frame.Minute = calendar.tm_min;
			frame.Second = calendar.tm_sec;
			frame.TimerMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
										  std::chrono::steady_clock::now() - ClockStart
			)
										  .count();
			GraphDeviceHost host({}, {}, std::span<const GraphDateTimeFrame>(&frame, 1));
			return host.Capture(call, out, failure);
		}
		if (call.Authored.Type == "pc.shell") {
			const auto *path = Get<std::string>(call, "path");
			if (!path || !Policy.AllowsName(*path)) {
				failure = "process capability refused by content policy";
				return false;
			}
			return Process(Processes, call, out, failure);
		}
		if (call.Authored.Type == "pc.http_request" || call.Authored.Type == "pc.http_request_file")
			return Http(Requests, Policy, call, out, failure);
		failure = "command host node type is unsupported";
		return false;
	}
}
