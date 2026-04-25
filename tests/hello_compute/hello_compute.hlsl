// SDL3 GPU D3D12 compute shader binding layout:
// (u[n], space1): Read-write storage textures, followed by read-write storage buffers

RWByteAddressBuffer inputBuffer : register(u0, space1);
RWByteAddressBuffer outputBuffer : register(u1, space1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint idx = dispatchThreadID.x;
    if (idx < 1024) {
        uint value = inputBuffer.Load(idx * 4);
        outputBuffer.Store(idx * 4, value * 2);
    }
}
