--!strict
-- Stable GPU compaction feeds an authored reusable kernel. Polling never waits.
local compute = game:GetService("ComputeService")
local capacity = 4097
local plan = compute:StockGraph("StableCompactU32", capacity)
table.insert(plan.Buffers, {Name="doubled", Type="u32", Capacity=capacity, CountBuffer="count"})
table.insert(plan.Buffers, {Name="double-errors", Type="u32", Capacity=65, Count=65})
table.insert(plan.ErrorBuffers, "double-errors")
table.insert(plan.Kernels, {
    Name="authored-double", Local={64,1,1},
    Ports={
        {Name="Values", Type="u32", Access="Read", Set=0, Binding=0},
        {Name="Count", Type="u32", Access="Read", Set=0, Binding=1, MinimumBytes=4},
        {Name="Result", Type="u32", Access="Write", Set=1, Binding=0},
        {Name="Errors", Type="u32", Access="Write", Set=1, Binding=1},
    },
    Source=[=[#version 450
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
]=],
})
table.insert(plan.Passes, {
    Name="authored-double", Kernel="authored-double", DispatchBuffer="packed",
    Bindings={{Port="Values",Buffer="packed"},{Port="Count",Buffer="count"},
        {Port="Result",Buffer="doubled"},{Port="Errors",Buffer="double-errors"}},
})
local ticket = compute:DefineGraph("example.compact-double", plan)
local sizes = {0,65,4097,7,65,4098}
local caseIndex = 0
local expected: {number} = {}
local connection
connection = game:GetService("RunService").Heartbeat:Connect(function()
    local reply = compute:PollGraph(ticket)
    if reply.Status == "pending" then return end
    if caseIndex == #sizes then
        assert(reply.Status == "failed" and reply.Code == "invalid element count", reply.Detail)
    else
        assert(reply.Status == "complete", reply.Detail)
        if caseIndex > 0 then
            local output = reply.Outputs[1]
            assert(output.Count == #expected)
            local bytes = compute:ReadGraphResult(ticket,"doubled",0,65536)
            assert(buffer.len(bytes) == #expected * 4)
            for index,value in expected do assert(buffer.readu32(bytes,(index-1)*4) == value) end
        end
    end
    assert(compute:ReleaseGraph(ticket))
    if caseIndex == #sizes then
        connection:Disconnect()
        local marker = Instance.new("Part")
        marker.Name = "BufferComputeComplete"
        marker.Anchored = true
        marker.Color = Color3.new(0.2,1,0.4)
        marker.Position = Vector3.new(0,3,0)
        marker.Parent = workspace
        return
    end
    caseIndex += 1
    local count = sizes[caseIndex]
    local bytes = buffer.create(count * 4)
    expected = {}
    for index=0,count-1 do
        local value = if caseIndex == 5 or index % 3 == 0 then 0 else index+1
        buffer.writeu32(bytes,index*4,value)
        if value ~= 0 then table.insert(expected,value*2) end
    end
    ticket = compute:SubmitGraph("example.compact-double", {
        Inputs={{Name="input",Type="u32",Count=count}},
        Outputs={{Name="doubled",MaximumElements=capacity}},
    }, bytes)
end)
