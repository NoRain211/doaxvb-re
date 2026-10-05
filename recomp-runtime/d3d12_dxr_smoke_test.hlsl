RaytracingAccelerationStructure scene : register(t0);

struct Result {
    uint hit;
    uint instance;
    uint primitive;
    float distance;
};
RWStructuredBuffer<Result> results : register(u0);
RWTexture2D<float4> image : register(u1);
cbuffer Frame : register(b0) {
    uint frameIndex;
    uint width;
    uint height;
};

Result traceRay(uint index)
{
    const float3 origins[4] = {
        float3(0.25, 0.25, 0), float3(2.25, 0.25, 0),
        float3(4, 4, 0), float3(0.25, 0.25, 0)
    };
    RayDesc ray;
    ray.Origin = origins[index];
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0;
    ray.TMax = index == 3 ? 0.5 : 10;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {}

    Result result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.instance = 0xffffffff;
    result.primitive = 0xffffffff;
    result.distance = -1;
    if (result.hit) {
        result.instance = query.CommittedInstanceID();
        result.primitive = query.CommittedPrimitiveIndex();
        result.distance = query.CommittedRayT();
    }
    return result;
}

[numthreads(4, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    results[thread.x] = traceRay(thread.x);
}

[numthreads(8, 8, 1)]
void imageMain(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= width || thread.y >= height) return;
    Result result = traceRay(thread.x % 4);
    uint stamp = (frameIndex * 17 + thread.x + thread.y * 3) & 255;
    image[thread.xy] = float4(result.hit ? 255 : 0,
        result.hit ? result.instance : 0, stamp, 255) / 255.0;
}
