float4 PSMain(float4 pos : SV_Position, float3 nrm : NORMAL, float3 col : COLOR0) : SV_Target
{
    return float4(col, 1);
}
