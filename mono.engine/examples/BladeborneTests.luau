--!strict

local Modules = script :: any
local Harness = require(Modules.BladeborneTests.Harness)
local CombatSolver = require(Modules.BladeborneCore.CombatSolver)
local Progression = require(Modules.BladeborneCore.Progression)
local SpatialIndex = require(Modules.BladeborneCore.SpatialIndex)
local BotMind = require(Modules.BladeborneCore.BotMind)

Harness.run({
	{
		Name = "four sword steps",
		Run = function()
			local state = CombatSolver.newState()
			local now = 0
			local expectedDamage = { 10, 10.5, 11, 15 }
			for step = 1, 4 do
				local accepted = CombatSolver.tryAttack(state, now)
				assert(accepted, `step {step} was refused`)
				now += 0.11
				local impact = CombatSolver.advance(state, now)
				assert(impact ~= nil, `step {step} produced no impact`)
				assert(impact.Step == step, `expected step {step}, got {impact.Step}`)
				assert(math.abs(impact.Damage - expectedDamage[step]) < 0.001, "wrong damage")
				now += 0.55
			end
		end,
	},
	{
		Name = "recovery and reset",
		Run = function()
			local state = CombatSolver.newState()
			assert(CombatSolver.tryAttack(state, 2))
			local accepted, reason = CombatSolver.tryAttack(state, 2.01)
			assert(not accepted and reason == "recovery", "recovery accepted an early input")
			assert(CombatSolver.tryAttack(state, 4))
			assert(state.Step == 1, "stale combo did not reset")
		end,
	},
	{
		Name = "nearest hostile target",
		Run = function()
			local target = CombatSolver.nearestTarget(
				Vector3.new(0, 0, 0),
				Vector3.new(1, 0, 0),
				"player",
				{
					{ Id = 8, Position = Vector3.new(4, 0, 0), Faction = "enemy", Alive = true },
					{ Id = 3, Position = Vector3.new(4, 0, 0), Faction = "enemy", Alive = true },
					{ Id = 1, Position = Vector3.new(-1, 0, 0), Faction = "enemy", Alive = true },
				}
			)
			assert(target ~= nil and target.Id == 3, "target tie was not deterministic")
		end,
	},
	{
		Name = "progression sanitizes and levels",
		Run = function()
			local profile = Progression.sanitize({
				Level = -5,
				Experience = 999,
				Gold = 20.8,
				Inventory = { RustSword = 1, Broken = -4 },
			})
			assert(profile.Level == 5, `expected level 5, got {profile.Level}`)
			assert(profile.Experience == 79, `expected 79 experience, got {profile.Experience}`)
			assert(profile.Gold == 20, "gold was not normalized")
			assert(Progression.grantItem(profile, "Broken", 0) == 0, "negative inventory survived")
			local levels = Progression.grantKill(profile, 2000, 12)
			assert(levels == 2, `expected two levels, got {levels}`)
			assert(profile.Kills == 1 and profile.Gold == 32, "kill rewards were not applied")
		end,
	},
	{
		Name = "bot targeting scales through one thousand actors",
		Run = function()
			for _, actorCount in { 10, 100, 1000 } do
				local actors = {}
				for id = 1, actorCount do
					table.insert(actors, {
						Id = id,
						Position = Vector3.new((id % 40) * 4, 0, math.floor(id / 40) * 4),
						Faction = if id % 2 == 0 then "husk" else "warden",
						Alive = true,
					})
				end
				local grid = SpatialIndex.build(actors, 18)
				local decisions = 0
				for _, actor in actors do
					local target = SpatialIndex.nearestEnemy(grid, actor, 72)
					assert(target ~= nil and target.Faction ~= actor.Faction, "spatial target was not hostile")
					BotMind.decide(actor.Id, actor.Position, target.Position, 2.5)
					decisions += 1
				end
				assert(decisions == actorCount, `only {decisions} of {actorCount} minds ran`)
			end
		end,
	},
})

local marker = Instance.new("Part")
marker.Name = "BladeborneTestsPassed"
marker.Anchored = true
marker.Parent = workspace
