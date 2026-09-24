#pragma once

// Owned, transactional registration for optional control-surface providers.
//
// A hook builds its rows privately, then publishes all of them at once. Its
// lease owns the rows until it closes, which keeps an optional host service
// from leaving callable callbacks behind after the service is gone.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::control {

	class Surface;
	struct Tool;
	struct Resource;
	struct Prompt;

	// One numeric bound a hook exposes through capability discovery.
	struct HookLimit {
		// Stable capability limit name.
		std::string Name;
		// Greatest value the active hook accepts.
		uint64_t Maximum = 0;
	};

	// The externally stable identity and metadata for one optional provider.
	struct HookDescriptor {
		// Stable provider discovery name.
		std::string Id;
		// Provider-owned contract revision.
		std::string Revision;
		// One sentence describing the provider's responsibility.
		std::string Purpose;
		// Providers that must remain active for this one to activate.
		std::vector<std::string> Dependencies;
		// Numeric bounds the provider declares to clients.
		std::vector<HookLimit> Limits;
	};

	// The lifetime state of one hook activation.
	enum class HookState : uint8_t { Inactive, Starting, Active, Draining, Failed };

	// Copyable discovery metadata for an installed hook.
	struct HookStatus {
		// Static metadata declared when the hook was activated.
		HookDescriptor Descriptor;
		// Current activation lifetime state.
		HookState State = HookState::Inactive;
		// Monotonic generation for this activation instance.
		uint64_t Generation = 0;
		// Callable names installed by this activation.
		std::vector<std::string> Tools;
		// Resource URIs installed by this activation.
		std::vector<std::string> Resources;
		// Prompt names installed by this activation.
		std::vector<std::string> Prompts;
	};

	struct HookRegistryState;

	// A generation-bearing activation owner. Closing it is safe to repeat.
	class HookLease final {
	  public:
		// Creates an empty lease.
		HookLease() = default;
		// Closes the activation if this lease still owns one.
		~HookLease();
		HookLease(const HookLease &) = delete;
		HookLease &operator=(const HookLease &) = delete;
		// Transfers ownership from another lease.
		HookLease(HookLease &&other) noexcept;
		// Releases this lease and takes another lease's ownership.
		HookLease &operator=(HookLease &&other) noexcept;

		// Removes this activation once calls already holding it have returned.
		void Close();
		// Whether this lease still names an active or draining activation.
		bool IsValid() const;
		// Stable provider identity.
		std::string_view Id() const;
		// Monotonic activation generation for this provider instance.
		uint64_t Generation() const;

	  private:
		friend class HookRegistry;
		HookLease(std::weak_ptr<HookRegistryState> state, std::string id, uint64_t generation);

		std::weak_ptr<HookRegistryState> State;
		std::string Id_;
		uint64_t Generation_ = 0;
	};

	// Rows accumulated during one activation before they become visible.
	class HookRegistration final {
	  public:
		// Registration transactions cannot be copied.
		HookRegistration(const HookRegistration &) = delete;
		HookRegistration &operator=(const HookRegistration &) = delete;
		// Moves a private registration transaction.
		HookRegistration(HookRegistration &&) noexcept;
		// Replaces this private registration transaction.
		HookRegistration &operator=(HookRegistration &&) noexcept;
		// Discards unpublished staged rows.
		~HookRegistration();

		// Stages one callable row. A repeated name refuses this activation.
		bool Add(Tool tool);
		// Stages one readable row. A repeated URI refuses this activation.
		bool Add(Resource resource);
		// Stages one prompt row. A repeated name refuses this activation.
		bool Add(Prompt prompt);
		// Why staging refused a row, or empty when it remains valid.
		std::string_view Failure() const;
		// Retains published rows after Close until this predicate reports terminal work drained.
		void SetDrain(std::function<bool()> drained);
		// Keeps one already registered cleanup tool callable while this hook drains.
		void KeepToolDuringDrain(std::string name);
		// Releases provider-owned callbacks after its rows are no longer reachable.
		void SetRelease(std::function<void()> release);

	  private:
		friend class HookRegistry;
		HookRegistration();
		struct Impl;
		std::unique_ptr<Impl> State;
	};

	// Stages one optional provider's rows for registration.
	using HookInstaller = std::function<void(HookRegistration &)>;

	// The surface-local owner table for optional provider rows.
	class HookRegistry final {
	  public:
		// Builds a registry bound to one surface.
		explicit HookRegistry(Surface &surface);
		// Detaches all outstanding leases from the surface.
		~HookRegistry();
		HookRegistry(const HookRegistry &) = delete;
		HookRegistry &operator=(const HookRegistry &) = delete;

		// Activates one provider after staging and validating all of its rows.
		HookLease Activate(HookDescriptor descriptor, const HookInstaller &installer, std::string &failure);
		// Lists every currently active or draining activation.
		std::vector<HookStatus> Active() const;
		// Monotonic revision of successful activation and removal changes.
		uint64_t ControlGeneration() const;
		// Whether an active hook owns this tool name.
		bool OwnsTool(std::string_view name) const;
		// Whether an active hook owns this resource URI.
		bool OwnsResource(std::string_view uri) const;
		// Whether an active hook owns this prompt name.
		bool OwnsPrompt(std::string_view name) const;
		// Returns whether an active hook exposes this tool.
		bool VisibleTool(std::string_view name) const;
		// Returns whether an active hook exposes this resource.
		bool VisibleResource(std::string_view uri) const;
		// Returns whether an active hook exposes this prompt.
		bool VisiblePrompt(std::string_view name) const;
		// Reaps hooks whose draining work has finished.
		void Pump() {
			Reap();
		}

	  private:
		friend class HookLease;
		friend class Surface;
		void Reap();
		void Close(std::string_view id, uint64_t generation);
		bool Holds(std::string_view tool, std::string_view expectedId, uint64_t expectedGeneration);
		bool HoldsResource(std::string_view uri, std::string_view expectedId, uint64_t expectedGeneration);
		bool HoldsPrompt(std::string_view name, std::string_view expectedId, uint64_t expectedGeneration);
		void Release(std::string_view id, uint64_t generation);

		std::shared_ptr<HookRegistryState> State;
	};
}
