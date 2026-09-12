--!strict

type PresetFactory = () -> { [string]: any }

local ArcaneBolt: PresetFactory = require(script.ArcaneBolt)
local AtomicNova: PresetFactory = require(script.AtomicNova)
local AtomicNovaCluster: PresetFactory = require(script.AtomicNovaCluster)
local Fireball: PresetFactory = require(script.Fireball)
local VenomDart: PresetFactory = require(script.VenomDart)
local GravityWell: PresetFactory = require(script.GravityWell)
local HealingBloom: PresetFactory = require(script.HealingBloom)
local PurifyingPulse: PresetFactory = require(script.PurifyingPulse)
local MeteorCluster: PresetFactory = require(script.MeteorCluster)
local MeteorChildMolten: PresetFactory = require(script.MeteorChildMolten)
local MeteorChildCinder: PresetFactory = require(script.MeteorChildCinder)
local ParticleTailShowcase: PresetFactory = require(script.ParticleTailShowcase)
local ChainLightning: PresetFactory = require(script.ChainLightning)
local ChainArcTwo: PresetFactory = require(script.ChainArcTwo)
local ChainArcThree: PresetFactory = require(script.ChainArcThree)
local ChainArcFour: PresetFactory = require(script.ChainArcFour)
local GlacialLance: PresetFactory = require(script.GlacialLance)
local Splinterstorm: PresetFactory = require(script.Splinterstorm)
local SplinterShard: PresetFactory = require(script.SplinterShard)

local canonicalFactories: { [string]: PresetFactory } = {
	ArcaneBolt = ArcaneBolt,
	AtomicNova = AtomicNova,
	Fireball = Fireball,
	VenomDart = VenomDart,
	GravityWell = GravityWell,
	HealingBloom = HealingBloom,
	PurifyingPulse = PurifyingPulse,
	MeteorCluster = MeteorCluster,
	ParticleTailShowcase = ParticleTailShowcase,
	ChainLightning = ChainLightning,
	GlacialLance = GlacialLance,
	Splinterstorm = Splinterstorm,
}
table.freeze(canonicalFactories)

local aliases: { [string]: PresetFactory } = {
	StraightBolt = ArcaneBolt,
	BallisticBurst = MeteorCluster,
	BezierSeekerVisual = VenomDart,
}
table.freeze(aliases)

local Presets = {}

function Presets.get(name: string): { [string]: any }
	local factory = canonicalFactories[name] or aliases[name]
	if factory == nil then
		error("unknown magic preset '" .. name .. "'", 2)
	end
	return factory()
end

-- Child recipes a root's burst reaches, keyed by the root's document id. Every factory
-- returns a fresh document, so callers can mutate what they get back.
local recipeFactoriesByRootId: { [string]: { PresetFactory } } = {
	meteor_cluster = { MeteorChildMolten, MeteorChildCinder },
	atomic_nova = { AtomicNovaCluster },
	-- The chain's links, in order. Every one is reachable from the root, so
	-- `compileGraph` needs all three or it reports MISSING_RECIPE at the first
	-- hop -- which is why the list is the whole chain rather than just the
	-- document the root names.
	chain_lightning = { ChainArcTwo, ChainArcThree, ChainArcFour },
	splinterstorm = { SplinterShard },
}
table.freeze(recipeFactoriesByRootId)

-- Returns fresh authored documents for one complete preset recipe graph.
function Presets.getGraph(name: string): { [string]: any }
	local root = Presets.get(name)
	local recipes = {}
	for _, factory in ipairs(recipeFactoriesByRootId[root.id] or {}) do
		local recipe = factory()
		recipes[recipe.id] = recipe
	end
	return { root = root, recipes = recipes }
end

function Presets.all(): { [string]: { [string]: any } }
	local result = {}
	for _, name in ipairs(Presets.names()) do
		result[name] = canonicalFactories[name]()
	end
	return result
end

function Presets.names(): { string }
	return {
		"ArcaneBolt",
		"AtomicNova",
		"ChainLightning",
		"Fireball",
		"GlacialLance",
		"GravityWell",
		"HealingBloom",
		"MeteorCluster",
		"ParticleTailShowcase",
		"PurifyingPulse",
		"Splinterstorm",
		"VenomDart",
	}
end

Presets.ArcaneBolt = ArcaneBolt
Presets.AtomicNova = AtomicNova
Presets.Fireball = Fireball
Presets.VenomDart = VenomDart
Presets.GravityWell = GravityWell
Presets.HealingBloom = HealingBloom
Presets.PurifyingPulse = PurifyingPulse
Presets.MeteorCluster = MeteorCluster
Presets.ParticleTailShowcase = ParticleTailShowcase
Presets.ChainLightning = ChainLightning
Presets.ChainArcTwo = ChainArcTwo
Presets.ChainArcThree = ChainArcThree
Presets.ChainArcFour = ChainArcFour
Presets.GlacialLance = GlacialLance
Presets.Splinterstorm = Splinterstorm
Presets.SplinterShard = SplinterShard
Presets.AtomicNovaCluster = AtomicNovaCluster
Presets.MeteorChildMolten = MeteorChildMolten
Presets.MeteorChildCinder = MeteorChildCinder
Presets.StraightBolt = ArcaneBolt
Presets.BallisticBurst = MeteorCluster
Presets.BezierSeekerVisual = VenomDart
Presets.Get = Presets.get
Presets.GetGraph = Presets.getGraph
Presets.All = Presets.all
Presets.Names = Presets.names

return table.freeze(Presets)
