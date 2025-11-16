struct ASPayload { uint dummy; };

[numthreads(1,1,1)]
void ASMain(in uint gtid : SV_GroupThreadID, out ASPayload payload)
{
    DispatchMesh(1,1,1, payload);
}
