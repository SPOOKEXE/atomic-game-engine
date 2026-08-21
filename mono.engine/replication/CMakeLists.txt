mono_add_library(replication
	TIER shared
	DEPS
		Engine::core

		# L2, `shared`. **Signing is a fan-out now.** `Authority::Resign` hashes
		# every carrier of every signed component every tick, which at twelve
		# thousand parts measured as a dozen even costs of about 0.3 ms and most
		# of `Authority::Publish`. The slots are independent, so they are signed
		# through `Jobs::For` - and a dependency a file includes is one its build
		# file says out loud.
		Engine::parallel
		Engine::ecs
		Engine::net
		Engine::world

		Engine::assets
)
