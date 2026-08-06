--!strict

-- Canonical serialized values used by authored and compiled spell data.
local Enums = {
	DamageType = table.freeze({
		Magic = "Magic",
		Fire = "Fire",
		Poison = "Poison",
		Arcane = "Arcane",
		Ice = "Ice",
		Physical = "Physical",
	}),
	EffectKind = table.freeze({
		Damage = "Effect.Damage",
		Heal = "Effect.Heal",
		Pull = "Effect.Pull",
		ApplyStatus = "Effect.ApplyStatus",
		Cleanse = "Effect.Cleanse",
		Area = "Effect.Area",
	}),
	StatusId = table.freeze({
		Burn = "burn",
		Poison = "poison",
		Chill = "chill",
		Laceration = "laceration",
	}),
	EffectTag = table.freeze({
		Area = "area",
		Fire = "fire",
		Burn = "burn",
		Poison = "poison",
		Projectile = "projectile",
		Meteor = "meteor",
		Arcane = "arcane",
		Spell = "spell",
		DamageOverTime = "damage_over_time",
		Ice = "ice",
		Slow = "slow",
		Bleed = "bleed",
		Physical = "physical",
	}),
	-- Stats a status modifier may address. `Validator` only checks that a stat
	-- is a well-formed id, because what a game keeps on an actor is the game's
	-- business -- these are the names this project's own content uses, so a typo
	-- is a diff rather than a silently inert modifier.
	Stat = table.freeze({
		MoveSpeed = "MoveSpeed",
		AttackSpeed = "AttackSpeed",
		DamageTaken = "DamageTaken",
	}),
	Trigger = table.freeze({
		Impact = "Impact",
		Expire = "Expire",
	}),
	AreaCenter = table.freeze({
		Impact = "Impact",
		Projectile = "Projectile",
		Source = "Source",
	}),
	AreaFalloff = table.freeze({
		Constant = "Constant",
		Linear = "Linear",
	}),
	Relation = table.freeze({
		Any = "Any",
		Self = "Self",
		Ally = "Ally",
		Enemy = "Enemy",
	}),
	EventAction = table.freeze({
		None = "None",
		Terminate = "Terminate",
		Burst = "Burst",
	}),
	BurstTrigger = table.freeze({
		Impact = "Impact",
		Expire = "Expire",
		ImpactOrExpire = "ImpactOrExpire",
	}),
	BurstDistribution = table.freeze({
		Sphere = "Sphere",
		Cone = "Cone",
		-- Horizontal fan around the burst point. Children launch level, so their
		-- authored local +Y is world up and a ballistic recipe lands them on a ring.
		Ring = "Ring",
		-- A hollow cone: children leave between `arcMinDegrees` and
		-- `arcMaxDegrees` off the parent's travel, never straight on and never
		-- sideways.
		--
		-- Cone with a small angle looks like the same thing and is not. A cone is
		-- solid, so most of its children come out near the axis and a chain built
		-- on one is a straight line with a wobble. Excluding the middle is what
		-- makes every hop turn, which is what a chain of lightning does.
		Arc = "Arc",
	}),
}

return table.freeze(Enums)
