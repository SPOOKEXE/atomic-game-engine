// Paired with BufferCompute.luau. One installed stock graph feeds authored GLSL.
const compute = game.GetService('ComputeService');
const capacity = 4097;
const plan = compute.StockGraph('StableCompactU32',capacity);
plan.Buffers.push({Name:'doubled',Type:'u32',Capacity:capacity,CountBuffer:'count'});
plan.Buffers.push({Name:'double-errors',Type:'u32',Capacity:65,Count:65});
plan.ErrorBuffers.push('double-errors');
plan.Kernels.push({
  Name:'authored-double',Local:[64,1,1],
  Ports:[
    {Name:'Values',Type:'u32',Access:'Read',Set:0,Binding:0},
    {Name:'Count',Type:'u32',Access:'Read',Set:0,Binding:1,MinimumBytes:4},
    {Name:'Result',Type:'u32',Access:'Write',Set:1,Binding:0},
    {Name:'Errors',Type:'u32',Access:'Write',Set:1,Binding:1},
  ],
  Source:`#version 450
layout(set = 2, binding = 0, std140) uniform ComputeParameters {
    uvec4 Dispatch;
    uvec4 Buffers[32];
} computeParameters;

layout(local_size_x = 64) in;
layout(set = 0, binding = 0, std430) readonly buffer Values { uint Data[]; } values;
layout(set = 0, binding = 1, std430) readonly buffer Count { uint Data[1]; } count;
layout(set = 1, binding = 0, std430) writeonly buffer Result { uint Data[]; } result;
layout(set = 1, binding = 1, std430) writeonly buffer Errors { uint Data[]; } errors;

shared uint groupError;

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint lane = gl_LocalInvocationID.x;
	uint block = gl_WorkGroupID.x;
	uint logicalCount = count.Data[0];
	if (lane == 0u) groupError = logicalCount > computeParameters.Buffers[0].y ||
		logicalCount > computeParameters.Buffers[16].y ? 1u : 0u;
	barrier();
	if (index < logicalCount && index < computeParameters.Buffers[0].y && index < computeParameters.Buffers[16].y) {
		uint value = values.Data[index];
		if (value > 0x7fffffffu) atomicOr(groupError, 1u);
		else result.Data[index] = value * 2u;
	}
	barrier();
	if (lane == 0u && block < computeParameters.Buffers[17].y) errors.Data[block] = groupError;
}
`,
});
plan.Passes.push({Name:'authored-double',Kernel:'authored-double',DispatchBuffer:'packed',
  Bindings:[{Port:'Values',Buffer:'packed'},{Port:'Count',Buffer:'count'},
    {Port:'Result',Buffer:'doubled'},{Port:'Errors',Buffer:'double-errors'}]});
let ticket = compute.DefineGraph('example.compact-double',plan);
const sizes = [0,65,4097,7,65,4098];
let caseIndex = -1;
let expected = [];
function check(condition,detail) { if (!condition) throw new Error(detail || 'compute result differs'); }
const connection = game.GetService('RunService').Heartbeat.Connect(() => {
  const reply = compute.PollGraph(ticket);
  if (reply.Status === 'pending') return;
  if (caseIndex === sizes.length-1) {
    check(reply.Status === 'failed' && reply.Code === 'invalid element count',reply.Detail);
  } else {
    check(reply.Status === 'complete',reply.Detail);
    if (caseIndex >= 0) {
      check(reply.Outputs[0].Count === expected.length);
      const bytes = compute.ReadGraphResult(ticket,'doubled',0,65536);
      check(bytes.byteLength === expected.length*4);
      const view = new DataView(bytes);
      expected.forEach((value,index) => check(view.getUint32(index*4,true) === value));
    }
  }
  check(compute.ReleaseGraph(ticket));
  if (caseIndex === sizes.length-1) {
    connection.Disconnect();
    const marker = Instance.new('Part');
    marker.Name = 'BufferComputeComplete'; marker.Anchored = true;
    marker.Color = Color3.new(0.2,1,0.4); marker.Position = Vector3.new(0,3,0);
    marker.Parent = workspace;
    return;
  }
  ++caseIndex;
  const count = sizes[caseIndex];
  const bytes = new ArrayBuffer(count*4);
  const view = new DataView(bytes);
  expected = [];
  for (let index=0;index<count;++index) {
    const value = caseIndex === 4 || index%3 === 0 ? 0 : index+1;
    view.setUint32(index*4,value,true);
    if (value !== 0) expected.push(value*2);
  }
  ticket = compute.SubmitGraph('example.compact-double',{
    Inputs:[{Name:'input',Type:'u32',Count:count}],
    Outputs:[{Name:'doubled',MaximumElements:capacity}],
  },bytes);
});
