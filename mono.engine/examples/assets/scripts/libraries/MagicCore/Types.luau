--!strict

-- Runtime constants and shared type names for the first spell-document slice.
-- Authored vectors are plain JSON-safe records, never Roblox Vector3 values.

export type Vector3 = {
	x: number,
	y: number,
	z: number,
}

export type NumberRange = {
	min: number,
	max: number,
}

export type DiagnosticSeverity = "Info" | "Warning" | "Error"

export type Diagnostic = {
	code: string,
	path: string,
	message: string,
	severity: DiagnosticSeverity,
}

export type LifetimeTime = {
	kind: "Lifetime.Time",
	seconds: number,
}

export type MotionLinear = {
	kind: "Motion.Linear",
	velocity: Vector3,
}

export type MotionBallistic = {
	kind: "Motion.Ballistic",
	velocity: Vector3,
	acceleration: Vector3,
}

export type MotionBezier = {
	kind: "Motion.Bezier",
	duration: number,
	space: "LaunchLocal" | "WorldSnapshot",
	points: { Vector3 },
}

export type Motion = MotionLinear | MotionBallistic | MotionBezier

export type ParticleEmitter = {
	kind: "Presentation.Particle",
	id: string,
	enabledSeconds: number,
	lifetime: NumberRange,
	rate: number?,
}

export type CurveKey = {
	time: number,
	value: number,
}

export type PresentationCurve = {
	kind: "Presentation.Curve",
	id: string,
	property: "Scale" | "Transparency",
	timeMode: "NormalizedLifetime" | "AbsoluteSeconds",
	keys: { CurveKey },
}

export type TimelineEmitParticles = {
	kind: "Timeline.EmitParticles",
	id: string,
	time: number,
	emitterId: string,
	count: number,
}

export type TimelineSetEmitterEnabled = {
	kind: "Timeline.SetEmitterEnabled",
	id: string,
	time: number,
	emitterId: string,
	enabled: boolean,
}

export type TimelineCue = TimelineEmitParticles | TimelineSetEmitterEnabled

export type Color = {
	r: number,
	g: number,
	b: number,
}

export type PresentationPart = {
	kind: "Presentation.Part",
	shape: "Ball" | "Block"?,
	size: Vector3?,
	color: Color?,
	transparency: number?,
	material: "Neon" | "SmoothPlastic" | "ForceField" | "Glass"?,
	particles: { ParticleEmitter }?,
	curves: { PresentationCurve }?,
	timeline: { TimelineCue }?,
}

export type ProjectileRecipe = {
	lifetime: LifetimeTime,
	motion: Motion,
	presentation: PresentationPart,
}

export type EventRecord = {
	kind: "Event.Spawn" | "Event.Impact" | "Event.Expire",
	id: string,
	order: number,
	action: "None" | "Terminate" | "Burst"?,
}

export type RandomConfig = {
	kind: "Random.Derived",
	seedSource: "Server",
	purpose: string,
}

export type BurstChildGroup = {
	id: string,
	recipeId: string,
	count: number,
}

export type Burst = {
	kind: "Burst.Deterministic",
	trigger: "Impact" | "Expire" | "ImpactOrExpire",
	distribution: "Sphere" | "Cone" | "Ring" | "Arc",
	coneAngleDegrees: number?,
	arcMinDegrees: number?,
	arcMaxDegrees: number?,
	children: { BurstChildGroup },
	maxDepth: number,
	random: RandomConfig,
}

export type EffectDamage = {
	kind: "Effect.Damage",
	amount: number,
	damageType: string,
	sourceTags: { string },
}

export type EffectHeal = {
	kind: "Effect.Heal",
	amount: number,
}

export type EffectPull = {
	kind: "Effect.Pull",
	center: "Impact" | "Projectile" | "Source",
	deltaSpeed: number,
	maxDeltaSpeed: number,
}

export type EffectApplyStatus = {
	kind: "Effect.ApplyStatus",
	definitionId: string,
	definitionVersion: number,
	stacks: number,
	potency: number,
	durationOverride: number?,
}

export type EffectCleanse = {
	kind: "Effect.Cleanse",
	includeAnyTags: { string },
	includeAllTags: { string },
	excludeAnyTags: { string },
	maxRemovals: number,
}

export type AtomicEffect = EffectDamage | EffectHeal | EffectPull | EffectApplyStatus | EffectCleanse

export type EffectArea = {
	kind: "Effect.Area",
	center: "Impact" | "Projectile" | "Source",
	radius: number,
	falloff: "Constant" | "Linear",
	relationToSource: "Any" | "Self" | "Ally" | "Enemy",
	includeAnyTags: { string },
	includeAllTags: { string },
	excludeAnyTags: { string },
	maxTargets: number,
	effects: { AtomicEffect },
}

export type Effect = AtomicEffect | EffectArea

export type EffectTrigger = {
	id: string,
	trigger: "Impact" | "Expire",
	order: number,
	effect: Effect,
	cueIds: { string }?,
}

export type StatusModifier = {
	id: string,
	stat: string,
	operation: "Add" | "Multiply",
	value: number,
}

export type StatusDefinition = {
	id: string,
	version: number,
	tags: { string },
	duration: number,
	tickInterval: number?,
	tickOnApply: boolean,
	tickAtExpiry: boolean,
	stackPolicy: "RefreshDuration" | "AddStacks" | "ReplaceIfStronger" | "Independent",
	maxStacks: number,
	maxIndependentInstances: number,
	periodicEffects: { EffectDamage | EffectHeal },
	modifiers: { StatusModifier },
	cueId: string?,
}

export type StatusInstance = {
	instanceId: string,
	definitionId: string,
	definitionVersion: number,
	sourceCastId: string,
	sourceActorId: string,
	targetActorId: string,
	appliedAt: number,
	expiresAt: number,
	nextTickTime: number?,
	stacks: number,
	potency: number,
	sequence: number,
}

export type Budgets = {
	maxProjectiles: number,
	maxEvents: number,
	maxParticles: number,
	maxBurstDepth: number,
	maxEffectCommands: number,
	maxAreaTargets: number,
	maxStatusesPerActor: number,
	maxStatusTicksPerAdvance: number,
}

export type SpellDocument = {
	schemaVersion: number,
	id: string,
	revision: number,
	projectile: ProjectileRecipe,
	burst: Burst?,
	events: { EventRecord }?,
	effectTriggers: { EffectTrigger }?,
	statusDefinitions: { StatusDefinition }?,
	budgets: Budgets?,
}

export type ExpandedBurstChild = {
	childIndex: number,
	groupId: string,
	recipeId: string,
}

export type Costs = {
	projectiles: number,
	events: number,
	particleEmissions: number,
	particleEmitters: number,
	burstChildren: number,
	effectCommands: number,
	areaTargets: number,
	statusInstances: number,
	statusTicks: number,
}

export type CompiledSpell = {
	schemaVersion: number,
	id: string,
	revision: number,
	projectileBlueprint: ProjectileRecipe,
	blueprint: ProjectileRecipe,
	serverProgram: { [string]: any },
	clientProgram: { [string]: any },
	burst: Burst?,
	events: { EventRecord },
	timeline: { TimelineCue },
	visualTail: number,
	costs: Costs,
	costEstimate: Costs,
	budgets: Budgets,
}

export type SpellDocumentGraph = {
	root: SpellDocument,
	recipes: { [string]: SpellDocument },
}

export type CompiledSpellGraph = {
	kind: "CompiledSpellGraph",
	schemaVersion: number,
	compilerVersion: number,
	rootId: string,
	root: CompiledSpell,
	recipes: { [string]: CompiledSpell },
	costs: Costs,
}

local Limits = table.freeze({
	MAX_ID_LENGTH = 64,
	MAX_STRING_LENGTH = 256,
	MAX_DOCUMENT_DEPTH = 32,
	MAX_DOCUMENT_VALUES = 5000,
	MAX_LIFETIME_SECONDS = 60,
	MAX_SPEED = 2000,
	MAX_ACCELERATION = 2000,
	MAX_VECTOR_COMPONENT = 1000000,
	MAX_PART_SIZE = 100,
	MAX_PARTICLES = 16,
	MAX_PARTICLE_RATE = 500,
	MAX_PARTICLE_LIFETIME = 10,
	MAX_CURVES = 8,
	MAX_CURVE_KEYS = 32,
	MAX_TIMELINE_CUES = 64,
	MAX_EVENTS = 64,
	MAX_BURST_GROUPS = 8,
	MAX_BURST_CHILDREN = 32,
	MAX_BURST_DEPTH = 4,
	MAX_EFFECT_TRIGGERS = 64,
	MAX_EFFECT_CUES_PER_TRIGGER = 16,
	MAX_EFFECTS_PER_AREA = 16,
	MAX_EFFECT_AMOUNT = 1000000,
	MAX_AREA_RADIUS = 2048,
	MAX_AREA_TARGETS = 128,
	MAX_PULL_SPEED = 4096,
	MAX_TAGS = 32,
	MAX_STATUS_DEFINITIONS = 128,
	MAX_STATUS_DURATION = 3600,
	MIN_STATUS_TICK_INTERVAL = 0.01,
	MAX_STATUS_TICKS_PER_INSTANCE = 4096,
	MAX_STATUS_STACKS = 100,
	MAX_INDEPENDENT_STATUS_INSTANCES = 32,
	MAX_PERIODIC_EFFECTS = 8,
	MAX_STATUS_MODIFIERS = 32,
	MAX_STATUS_LEDGER_ENTRIES = 8192,
	MAX_PROJECTILE_BUDGET = 4096,
	MAX_EVENT_BUDGET = 4096,
	MAX_PARTICLE_BUDGET = 100000,
	MAX_EFFECT_COMMAND_BUDGET = 100000,
	MAX_STATUS_BUDGET = 128,
	MAX_STATUS_TICKS_PER_ADVANCE_BUDGET = 4096,
	COST_SATURATION = 1000000000,
})

local DefaultBudgets = table.freeze({
	maxProjectiles = 64,
	maxEvents = 128,
	maxParticles = 2048,
	maxBurstDepth = 2,
	maxEffectCommands = 128,
	maxAreaTargets = 32,
	maxStatusesPerActor = 16,
	maxStatusTicksPerAdvance = 64,
})

return table.freeze({
	SCHEMA_VERSION = 1,
	COMPILER_VERSION = 1,
	Limits = Limits,
	DefaultBudgets = DefaultBudgets,
})
