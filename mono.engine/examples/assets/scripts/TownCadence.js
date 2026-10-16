// Paired with TownCadence.luau. Both scripts build the same authored town and use
// the same fixed-tick cadence and combat deadlines through one service.

const RunService = game.GetService('RunService');
if (!RunService.IsReplica()) {
  const cadence = game.GetService('UpdateCadenceService');
  const data = game.GetService('DataSceneService');
  const RIG = 'examples.SwingRig';
  const SOURCE_REVISION = 1;
  // Set the authored attribute before running this script to open one crowded town.
  const authoredPopulation = workspace.GetAttribute('CadencePopulation');
  if (authoredPopulation != null && authoredPopulation !== 2 && authoredPopulation !== 64) {
    throw new Error('CadencePopulation must be 2 or 64');
  }
  const crowded = authoredPopulation === 64;

  function part(name, size, position, color) {
    const object = Instance.new('Part');
    object.Name = name;
    object.Size = size;
    object.Position = position;
    object.Color = color;
    object.Anchored = true;
    object.Parent = workspace;
    return object;
  }

  part('Town Square', crowded ? Vector3.new(264, 1, 128) : Vector3.new(52, 1, 52),
    Vector3.new(0, -0.5, 0), Color3.new(0.29, 0.31, 0.29));
  for (let index = -1; index <= 1; ++index) {
    part('House ' + index, Vector3.new(7, 7, 7), Vector3.new(index * 14, 3.5, crowded ? 40 : -21),
      Color3.new(0.49, 0.36, 0.25));
  }

  function practiceTarget(name, id, position) {
    const target = part(name, Vector3.new(3, 4, 3), position, Color3.new(0.7, 0.3, 0.2));
    target.SetAttribute('DataFactoryId', id);
    const health = Instance.new('Humanoid');
    health.RootPart = target;
    health.MaxHealth = 100;
    health.Health = 100;
    health.Parent = target;
    return {target, health, id};
  }
  const sharedTarget = crowded ? null : practiceTarget('Practice Target', 'town.practice-target',
    Vector3.new(0, 2, -8));

  // Animation.luau's SwingRig clip layout, written through JS's byte view.
  const clip = Instance.new('AnimationBuffer');
  clip.Name = 'Town Walk';
  if (clip.KeyframeBytes !== 36) throw new Error('unexpected keyframe layout');
  const frames = new ArrayBuffer(clip.KeyframeBytes * 3);
  const view = new DataView(frames);
  function keyframe(index, time, angle) {
    const offset = index * clip.KeyframeBytes;
    view.setUint16(offset, 0, true);
    view.setUint16(offset + 2, 0, true);
    view.setFloat32(offset + 4, time, true);
    view.setFloat32(offset + 8, 0, true);
    view.setFloat32(offset + 12, 0, true);
    view.setFloat32(offset + 16, 0, true);
    view.setFloat32(offset + 20, 0, true);
    view.setFloat32(offset + 24, Math.sin(angle * 0.5), true);
    view.setFloat32(offset + 28, 0, true);
    view.setFloat32(offset + 32, Math.cos(angle * 0.5), true);
  }
  keyframe(0, 0, -0.3);
  keyframe(1, 0.5, 0.3);
  keyframe(2, 1, -0.3);
  if (!clip.BakeAnimation(1, frames)) throw new Error('clip bake failed');
  clip.Parent = workspace;

  const citizens = [];
  function citizen(name, x, color, practice) {
    const model = Instance.new('Model');
    model.Name = name;
    const rig = Instance.new('SkinnedMeshPart');
    rig.Name = 'Body';
    rig.MeshId = 'engine.SkinnedWedge';
    rig.RigId = RIG;
    rig.JointCount = 1;
    rig.Size = Vector3.new(2.5, 3, 2.5);
    rig.Position = Vector3.new(x, 2.5, 8);
    rig.Color = color;
    rig.Anchored = false;
    rig.Parent = model;
    const bone = Instance.new('Bone');
    bone.Name = 'Root';
    bone.Parent = rig;
    const animator = Instance.new('Animator');
    animator.Rig = rig;
    animator.Parent = rig;
    const animation = Instance.new('Animation');
    animation.AnimationBuffer = clip;
    animation.RigId = RIG;
    animation.Parent = animator;
    const track = Instance.new('AnimationTrack');
    track.Animation = animation;
    track.Looped = true;
    track.IsPlaying = true;
    track.WeightCurrent = 1;
    track.WeightTarget = 1;
    track.Parent = animator;
    const humanoid = Instance.new('Humanoid');
    humanoid.RootPart = rig;
    humanoid.WalkSpeed = 3.5;
    humanoid.Parent = model;
    // Keep the cosmetic source still while the physical body follows its target.
    const beacon = Instance.new('Part');
    beacon.Name = 'Beacon';
    beacon.Size = Vector3.new(0.2, 0.2, 0.2);
    beacon.Position = Vector3.new(x, 4.8, 8);
    beacon.Anchored = true;
    beacon.CanCollide = false;
    beacon.CanQuery = false;
    beacon.Transparency = 1;
    beacon.Parent = model;
    const from = Instance.new('Attachment');
    from.CFrame = CFrame.new(Vector3.new(-1.2, 0, -0.5));
    from.Parent = beacon;
    const to = Instance.new('Attachment');
    to.CFrame = CFrame.new(Vector3.new(1.2, 0.2, -0.5));
    to.Parent = beacon;
    const beam = Instance.new('Beam');
    beam.Name = 'Cosmetic Beam';
    beam.Enabled = true;
    beam.Attachment0 = from;
    beam.Attachment1 = to;
    beam.CurveSize0 = 0.6;
    beam.CurveSize1 = -0.4;
    beam.Width0 = 0.22;
    beam.Width1 = 0.35;
    beam.TextureSpeed = 0.25;
    beam.Color = ColorSequence.new(color);
    beam.Parent = beacon;
    model.Parent = workspace;
    if (!cadence.Configure(model, 'town.citizen.' + name, 8, 12, 4, 8, 3, 4, 8, 4, 8)) {
      throw new Error('cadence configuration failed');
    }
    model.SetAttribute('NextAttackTick', 0);
    citizens.push({model, rig, humanoid, target: practice.target,
      targetHumanoid: practice.health, targetId: practice.id});
  }

  if (crowded) {
    for (let index = 1; index <= 64; ++index) {
      const x = (index - 32.5) * 4;
      const practice = practiceTarget('Practice Target ' + index, 'town.practice-target.' + index,
        Vector3.new(x, 2.5, -492));
      citizen('Citizen ' + index, x, Color3.new(0.2, 0.62, 0.78), practice);
    }
  } else {
    citizen('North Guard', -9, Color3.new(0.2, 0.62, 0.78), sharedTarget);
    citizen('South Guard', 9, Color3.new(0.78, 0.6, 0.24), sharedTarget);
  }

  let tick = 0;
  RunService.Heartbeat.Connect(() => {
    tick += 1;
    for (const npc of citizens) {
      const target = npc.target;
      const targetHumanoid = npc.targetHumanoid;
      const difference = target.Position.sub(npc.rig.Position);
      const distance = difference.Magnitude;
      const fighting = distance <= 12 && targetHumanoid.Health > 0;
      if (!cadence.SetCombat(npc.model, fighting)) throw new Error('combat policy failed');

      const seeing = cadence.BeginUpdate(npc.model, 'perception', SOURCE_REVISION);
      if (seeing.due) {
        let visible = false;
        if (distance > 3) {
          const direction = difference.Unit;
          const hit = data.Raycast({
            origin: npc.rig.Position.add(direction.mul(2)),
            direction,
            max_distance_metres: distance,
          });
          visible = hit.status === 'ok' && hit.hit && hit.id === npc.targetId;
        }
        if (!cadence.CommitPerception(npc.model, seeing.token, SOURCE_REVISION, visible,
          target.Position, true)) throw new Error('perception commit failed');
      }

      const moving = cadence.BeginUpdate(npc.model, 'behaviour', SOURCE_REVISION);
      if (moving.due) {
        const observed = cadence.Perception(npc.model);
        const direction = observed.visible && distance > 5 ? difference.Unit : Vector3.zero;
        if (!cadence.CommitBehaviour(npc.model, moving.token, SOURCE_REVISION, direction, true)) {
          throw new Error('behaviour commit failed');
        }
      }

      // Deadline checks run on every tick. Combat first refreshes both domains.
      const nextAttack = npc.model.GetAttribute('NextAttackTick') || 0;
      if (fighting && tick >= nextAttack && cadence.CombatReady(npc.model, SOURCE_REVISION)) {
        targetHumanoid.Health = Math.max(0, targetHumanoid.Health - 1);
        npc.model.SetAttribute('NextAttackTick', tick + 30);
      }
    }
  });
}
