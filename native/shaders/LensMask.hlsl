// Pixel shader of the lens mask twin: marks every pixel of the scope glass with 1.
// Compiled ahead of time to DXIL by tools/make_shaders.py (the game's own shaders are DXIL,
// and one pipeline can't mix DXIL with the older DXBC format).
float4 main() : SV_Target
{
    return 1.0;
}
