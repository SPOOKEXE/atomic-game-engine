#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/world/PresentationRelay.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>

namespace engine::world {
	PresentationRelay::PresentationRelay(
		Universe &universe, WorldId world, uint64_t childSession, std::vector<std::string> channels
	)
		: Worlds(universe), World(world), ChildSession(childSession), Channels(std::move(channels)) {}
	PresentationRelay::~PresentationRelay() {
		Close();
	}
	void PresentationRelay::Close() {
		for (const auto &binding : Bindings)
			(void)Worlds.ClosePresentation(binding.Local);
		Bindings.clear();
		Returns.clear();
		Requests.clear();
		Replies.clear();
		PendingSize = {};
	}
	PresentationStatus PresentationRelay::Apply(const PresentationDirectory &directory) {
		core::ByteWriter validation;
		if (!WritePresentationDirectory(validation, directory) || directory.Session != ChildSession ||
			Worlds.IsRemote(World) || !Worlds.NameOf(World).IsValid())
			return PresentationStatus::Invalid;
		if (directory.Revision <= ChildRevision) return PresentationStatus::StaleEndpoint;
		for (const auto &address : directory.Endpoints) {
			if (address.World != Worlds.NameOf(World).Text() ||
				std::find(Channels.begin(), Channels.end(), address.Channel) == Channels.end()) {
				ENGINE_WARN(
					"presentation child for {} claimed ungranted endpoint {}/{}",
					Worlds.NameOf(World).Text(),
					address.World,
					address.Channel
				);
				return PresentationStatus::WrongHost;
			}
			const auto existing = Worlds.LookupPresentation(World, address.Channel);
			if (existing.Session && std::none_of(Bindings.begin(), Bindings.end(), [&](const auto &binding) {
					return binding.Local == existing;
				})) {
				ENGINE_WARN(
					"presentation child for {} collided with endpoint {}/{} session {} generation {}",
					Worlds.NameOf(World).Text(),
					address.World,
					address.Channel,
					existing.Session,
					existing.Generation
				);
				return PresentationStatus::WrongHost;
			}
		}
		std::erase_if(Bindings, [&](const auto &binding) {
			if (std::find(directory.Endpoints.begin(), directory.Endpoints.end(), binding.Child) !=
				directory.Endpoints.end())
				return false;
			(void)Worlds.ClosePresentation(binding.Local);
			return true;
		});
		for (const auto &address : directory.Endpoints) {
			if (std::any_of(Bindings.begin(), Bindings.end(), [&](const auto &binding) {
					return binding.Child == address;
				}))
				continue;
			const auto opened = Worlds.OpenPresentation(World, core::Name(address.Channel));
			if (opened.Status != PresentationStatus::Ok) {
				Close();
				return opened.Status;
			}
			Bindings.push_back({address, opened.Address, 0});
		}
		ChildRevision = directory.Revision;
		return PresentationStatus::Ok;
	}
	PresentationDirectory PresentationRelay::Routes() {
		// RoutesFor({}) contains remote endpoints; the local directory supplies
		// local consumers. Its revision already covers both sets.
		auto directory = Worlds.PresentationRoutesFor({});
		const auto local = Worlds.LocalPresentationDirectory();
		directory.Endpoints.insert(directory.Endpoints.end(), local.Endpoints.begin(), local.Endpoints.end());
		const auto world = Worlds.NameOf(World).Text();
		const auto collision = [&](std::string_view candidate) {
			return candidate.empty() || candidate == world ||
				   std::any_of(
					   directory.Endpoints.begin(),
					   directory.Endpoints.end(),
					   [candidate](const auto &address) { return address.World == candidate; }
				   );
		};
		if (collision(ReturnWorld)) {
			uint32_t suffix = 0;
			do {
				ReturnWorld = "$presentation-return." + std::to_string(suffix++);
			} while (collision(ReturnWorld));
		}
		Returns.clear();
		std::vector<PresentationAddress> forwarded;
		for (const auto &address : directory.Endpoints) {
			if (std::any_of(Bindings.begin(), Bindings.end(), [&](const auto &binding) {
					return binding.Local == address;
				}))
				continue;
			auto target = address;
			// A consumer in this same named world is remote to the child replica.
			// An opaque return alias keeps its route from claiming the child's world.
			if (target.World == world) target.World = ReturnWorld;
			Returns.push_back({address, target});
			forwarded.push_back(std::move(target));
		}
		directory.Endpoints = std::move(forwarded);
		return directory;
	}
	bool PresentationRelay::Retain(std::vector<PresentationMessage> &queue, PresentationMessage message) {
		const PresentationLimits limits;
		if (PendingSize.Messages >= limits.Messages || message.Payload.size() > limits.MaximumPayload ||
			message.Payload.size() > limits.Bytes - PendingSize.Bytes) {
			++Refusals;
			return false;
		}
		PendingSize.Messages++;
		PendingSize.Bytes += message.Payload.size();
		queue.push_back(std::move(message));
		return true;
	}
	void PresentationRelay::FlushReplies() {
		size_t taken = 0;
		for (const auto &message : Replies) {
			const auto binding = std::find_if(Bindings.begin(), Bindings.end(), [&](const auto &item) {
				return item.Child == message.From;
			});
			const auto route = std::find_if(Returns.begin(), Returns.end(), [&](const auto &item) {
				return item.Forwarded == message.To;
			});
			PresentationStatus status = PresentationStatus::StaleEndpoint;
			if (binding != Bindings.end() && route != Returns.end())
				status = Worlds.SendPresentation(
					World, binding->Local, route->Original, message.Correlation, message.Payload
				);
			if (status == PresentationStatus::Full) break;
			if (status != PresentationStatus::Ok) ++Refusals;
			PendingSize.Bytes -= message.Payload.size();
			PendingSize.Messages--;
			++taken;
		}
		Replies.erase(Replies.begin(), Replies.begin() + taken);
	}
	void PresentationRelay::FlushRequests(HostLink &child) {
		size_t taken = 0;
		for (const auto &message : Requests) {
			const auto binding = std::find_if(Bindings.begin(), Bindings.end(), [&](const auto &item) {
				return item.Local == message.To;
			});
			const auto route = std::find_if(Returns.begin(), Returns.end(), [&](const auto &item) {
				return item.Original == message.From;
			});
			if (binding != Bindings.end() && route != Returns.end()) {
				auto forwarded = message;
				forwarded.From = route->Forwarded;
				forwarded.To = binding->Child;
				if (!child.SendPresentation(forwarded)) break;
			} else
				++Refusals;
			PendingSize.Bytes -= message.Payload.size();
			PendingSize.Messages--;
			++taken;
		}
		Requests.erase(Requests.begin(), Requests.begin() + taken);
	}
	bool PresentationRelay::Pump(HostLink &child) {
		ENGINE_PROFILE("presentation relay");
		if (Worlds.TickExchangeFrameOpen()) return true;
		if (!child.Connected()) {
			Close();
			return false;
		}
		std::vector<HostFrame> frames;
		child.Receive(frames);
		for (auto &frame : frames) {
			if (frame.Signal == HostSignal::PresentationDirectory) {
				const auto status = Apply(frame.Directory);
				if (status != PresentationStatus::Ok && status != PresentationStatus::StaleEndpoint) {
					ENGINE_WARN(
						"presentation directory for {} refused with status {}, session {} expected {}, "
						"revision {}, endpoints {}",
						Worlds.NameOf(World).Text(),
						static_cast<unsigned>(status),
						frame.Directory.Session,
						ChildSession,
						frame.Directory.Revision,
						frame.Directory.Endpoints.size()
					);
					++Refusals;
					Close();
					return false;
				}
			} else if (frame.Signal == HostSignal::Presentation) {
				auto &message = frame.Presentation;
				const auto binding = std::find_if(Bindings.begin(), Bindings.end(), [&](const auto &item) {
					return item.Child == message.From;
				});
				if (binding == Bindings.end() || message.Sequence <= binding->ReceivedSequence) {
					++Refusals;
					continue;
				}
				const auto sequence = message.Sequence;
				if (Retain(Replies, std::move(message))) binding->ReceivedSequence = sequence;
			} else if (frame.Signal != HostSignal::Ready && frame.Signal != HostSignal::Heartbeat)
				++Refusals;
		}
		if (!child.Connected()) {
			Close();
			return false;
		}
		const auto routes = Routes();
		FlushReplies();
		if (!child.PublishPresentationRoutes(routes)) return true;
		for (const auto &binding : Bindings)
			for (auto &message : Worlds.TakePresentation(binding.Local))
				(void)Retain(Requests, std::move(message));
		FlushRequests(child);
		return true;
	}
}
