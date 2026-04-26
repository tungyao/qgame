// Compute shader: doubles input values
// Compatible with DXC -spirv and DXIL output

RWStructuredBuffer<uint> inputBuffer : register(u0, space1);
RWStructuredBuffer<uint> outputBuffer : register(u1, space1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint idx = dispatchThreadID.x;
    if (idx < 1024) {
        outputBuffer[idx] = inputBuffer[idx] * 2u;
    }
}
