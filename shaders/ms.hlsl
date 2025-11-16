struct VertexIn { float3 pos; float3 nrm; uint4 boneIdx; float4 boneWgt; };
struct MeshletData { uint vCount, pCount; uint vOffset, pOffset; uint boneBase; };

StructuredBuffer<VertexIn>      gVertices : register(t0);
StructuredBuffer(uint3)         gTris     : register(t1);
StructuredBuffer<MeshletData>   gMeshlets : register(t2);
StructuredBuffer(float4x4)      gBones    : register(t3);

cbuffer Globals : register(b0) { float4x4 gMVP; float gTime; float3 _pad; }

struct VSOut { float4 posH : SV_Position; float3 nrmW : NORMAL; float3 col : COLOR0; };

[outputtopology("triangle")]
[numthreads(64,1,1)]
void MSMain(in uint gtid : SV_GroupThreadID,
            out vertices VSOut vOut[256],
            out indices  uint3  pOut[256])
{
    MeshletData m = gMeshlets[0];
    if (gtid == 0) SetMeshOutputCounts(m.vCount, m.pCount);
    GroupMemoryBarrierWithGroupSync();

    if (gtid < m.vCount)
    {
        VertexIn vin = gVertices[m.vOffset + gtid];
        // 2-bone LBS for demo. For real content, use the vin.boneIdx to index gBones.
        float4x4 B0 = gBones[0];
        float4x4 B1 = gBones[1];

        float4 p = float4(vin.pos, 1);
        float3 n = vin.nrm;

        float4 skP = vin.boneWgt.x * mul(B0, p) + vin.boneWgt.y * mul(B1, p);
        float3 skN = normalize(vin.boneWgt.x * mul((float3x3)B0, n) + vin.boneWgt.y * mul((float3x3)B1, n));

        VSOut o; o.posH = mul(gMVP, skP); o.nrmW = skN; o.col = 0.5 + 0.5*skN;
        vOut[gtid] = o;
    }

    GroupMemoryBarrierWithGroupSync();

    if (gtid < m.pCount)
    {
        uint3 tri = gTris[m.pOffset + gtid];
        pOut[gtid] = tri;
    }
}
