// ─────────────────────────────────────────────────────────────────────────────
// wboit_composite.vert.hlsl
//
// Fullscreen triangle vertex shader for compositing the WBOIT accum + reveal
// textures onto the final render target.  Uses a single triangle that covers
// clip space [-1,1]² without any vertex buffer.
// ─────────────────────────────────────────────────────────────────────────────

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOutput main(uint vid : SV_VertexID)
{
    // Fullscreen triangle: 3 vertices cover clip space.
    //   vid 0 → (-1, -3)   bottom-left, outside viewport
    //   vid 1 → (-1,  1)   top-left
    //   vid 2 → ( 3,  1)   right of viewport
    // Together they form a triangle that covers the entire [-1,1]² square.
    float2 pos = float2(
        (vid == 2) ?  3.0 : -1.0,
        (vid == 0) ? -3.0 :  1.0
    );

    VSOutput o;
    o.position = float4(pos, 0.0, 1.0);

    // Map clip-space position to [0,1] UV.
    o.uv = float2(
        (vid == 2) ? 2.0 : 0.0,
        (vid == 0) ? 2.0 : 0.0
    );

    return o;
}
