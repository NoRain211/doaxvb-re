RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> depthImage : register(t1);
Texture2D<float4> colorImage : register(t2);
Texture2D<float4> previousGI : register(t3);
Texture2D<float4> previousPosition : register(t4);
Texture2D<float4> tracedGI : register(t5);
Texture2D<float4> accumulatedGI : register(t6);
Texture2D<float4> surfacePosition : register(t7);
Texture2D<float4> surfaceNormal : register(t8);
Texture2D<float4> previousNormal : register(t9);
StructuredBuffer<float3> positions : register(t10);
StructuredBuffer<uint> indices : register(t11);
StructuredBuffer<float2> uvs : register(t12);
StructuredBuffer<uint> triangles : register(t13);
struct Material { uint slice; uint test; float alpha; float factor; };
StructuredBuffer<Material> materials : register(t14);
Texture2DArray<float> alphaImage : register(t15);
SamplerState alphaWrap : register(s0);
RWTexture2D<float4> traceOutput : register(u0);
RWTexture2D<float4> historyOutput : register(u1);
RWTexture2D<float4> giOutput : register(u2);
RWTexture2D<float4> positionOutput : register(u3);
RWTexture2D<float4> normalOutput : register(u4);

cbuffer Frame : register(b0) {
    row_major float4x4 inverseViewProjection;
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    uint2 size;
    uint2 fullSize;
    uint frameIndex;
    uint historyValid;
    float2 padding;
    row_major float4x4 rayInverseViewProjection;
    float4 eyePosition;
};

cbuffer Filter : register(b1) { uint filterStep; };

// World units are metres. The sky shell sits kilometres out; a body is ~1.7 m.
// Decals, wet sand and shadow layers lie millimetres above the surface below
// them; hits nearer than layerDistance are those layers, not occluders.
static const float skyDistance = 200, contactDistance = 0.5, layerDistance = 0.05;
static const float skyLevel = 0.5;

float luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

int2 bounded(int2 p) { return clamp(p, int2(0, 0), int2(size) - 1); }

float3 unproject(float2 uv, float depth)
{
    float4 world = mul(float4(uv * float2(2, -2) + float2(-1, 1), depth, 1),
        inverseViewProjection);
    return world.xyz / world.w;
}

float3 worldPosition(int2 p, float depth)
{
    // Match the full-resolution texel selected by the D3D11 reduction pass.
    float2 uv = (floor((float2(p) + 0.5) * fullSize / size) + 0.5) / fullSize;
    return unproject(uv, depth);
}

float3 normalAt(int2 p)
{
    float d = depthImage[p];
    int2 l = bounded(p - int2(1, 0)), r = bounded(p + int2(1, 0));
    int2 u = bounded(p - int2(0, 1)), b = bounded(p + int2(0, 1));
    float3 w = worldPosition(p, d);
    float3 dx = abs(depthImage[r] - d) < abs(depthImage[l] - d)
        ? worldPosition(r, depthImage[r]) - w : w - worldPosition(l, depthImage[l]);
    float3 dy = abs(depthImage[b] - d) < abs(depthImage[u] - d)
        ? worldPosition(b, depthImage[b]) - w : w - worldPosition(u, depthImage[u]);
    float3 n = cross(dx, dy);
    n *= rsqrt(max(dot(n, n), 1e-20));
    float3 towardEye = worldPosition(p, 0) - w;
    return dot(n, towardEye) < 0 ? -n : n;
}

uint hash(uint v)
{
    v ^= v >> 16;
    v *= 0x7feb352d;
    v ^= v >> 15;
    v *= 0x846ca68b;
    return v ^ (v >> 16);
}

bool project(float3 world, float4x4 projection, out float2 uv, out float depth)
{
    float4 clip = mul(float4(world, 1), projection);
    uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    depth = clip.z / clip.w;
    return clip.w > 0 && all(uv >= 0) && all(uv < 1) && depth >= 0 && depth < 1;
}

bool depthAgrees(float a, float b)
{
    // Compare distance from the far plane, retaining precision near depth 1.
    return b < 1 && abs(a - b) <= max(2e-5, 0.025 * (1 - a));
}

float alphaUV(float uv, uint mode)
{
    if (mode == 0) return uv;
    if (mode == 1) uv = 1 - abs(2 * frac(uv * 0.5) - 1);
    // Clamp to atlas texel centers so the wrap sampler cannot cross an edge.
    return clamp(uv, 0.5 / 256, 1 - 0.5 / 256);
}

bool alphaPass(uint primitive, float2 barycentrics)
{
    uint index = triangles[primitive];
    if (index == 0xffffffff) return true;
    Material material = materials[index];
    uint first = 3 * primitive;
    float2 a = uvs[indices[first]], b = uvs[indices[first + 1]], c = uvs[indices[first + 2]];
    float2 uv = a + barycentrics.x * (b - a) + barycentrics.y * (c - a);
    uv = float2(alphaUV(uv.x, (material.test >> 11) & 3), alphaUV(uv.y, (material.test >> 13) & 3));
    precise float alpha = alphaImage.SampleLevel(alphaWrap, float3(uv, material.slice), 0);
    alpha = (material.test & (1u << 15)) ? material.alpha : alpha * material.alpha;
    alpha = (material.test & (1u << 16)) ? material.factor : alpha * material.factor;
    alpha = round(saturate(alpha) * 255);
    uint func = material.test & 7, ref = (material.test >> 3) & 255;
    return func == 7 || (func == 1 && alpha < ref) || (func == 2 && alpha == ref) ||
        (func == 3 && alpha <= ref) || (func == 4 && alpha > ref) ||
        (func == 5 && alpha != ref) || (func == 6 && alpha >= ref);
}

void primarySurface(int2 p, float depth, inout float3 world, inout float3 normal)
{
    float2 uv = (float2(p) + 0.5) / size;
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float4 nearClip = mul(float4(ndc, 0, 1), rayInverseViewProjection);
    float4 midClip = mul(float4(ndc, 0.5, 1), rayInverseViewProjection);
    float3 nearPoint = nearClip.xyz / nearClip.w;
    // The perspective eye maps to clip (0,0,z,0). For an orthographic camera
    // it is at infinity, so start the parallel ray on the near plane instead.
    RayDesc ray;
    ray.Origin = eyePosition.w != 0 ? eyePosition.xyz : nearPoint;
    ray.Direction = normalize(midClip.xyz / midClip.w - nearPoint);
    ray.TMin = eyePosition.w != 0 ? length(nearPoint) : 0;
    ray.TMax = 1e7;
    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alphaPass(query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return;
    uint first = 3 * query.CommittedPrimitiveIndex();
    float3 a = positions[indices[first]], b = positions[indices[first + 1]], c = positions[indices[first + 2]];
    float2 barycentrics = query.CommittedTriangleBarycentrics();
    float3 hit = a + barycentrics.x * (b - a) + barycentrics.y * (c - a);
    float2 hitUV;
    float hitDepth;
    // Allow a few D24 steps and the small center/sample offset of reduction,
    // but do not substitute scene geometry behind an unrecorded raster draw.
    if (!project(hit, viewProjection, hitUV, hitDepth) ||
        abs(hitDepth - depth) > max(4.0 / 16777215.0, 0.025 * (1 - depth))) return;
    world = hit;
    normal = normalize(cross(b - a, c - a));
    if (dot(normal, -ray.Direction) < 0) normal = -normal;
}

[numthreads(8, 8, 1)]
void traceMain(uint3 thread : SV_DispatchThreadID)
{
    int2 p = thread.xy;
    if (any(thread.xy >= size)) return;
    float3 radiance = 0;
    float d = depthImage[p];
    positionOutput[p] = float4(0, 0, 0, 1);
    normalOutput[p] = 0;
    if (d < 1) {
        float3 world = worldPosition(p, d), n = normalAt(p);
        primarySurface(p, d, world, n);
        float3 reconstructed = worldPosition(p, d);
        float pixelSize = max(length(worldPosition(bounded(p + int2(1, 0)), d) - reconstructed),
            length(worldPosition(bounded(p + int2(0, 1)), d) - reconstructed));
        positionOutput[p] = float4(world, d);
        normalOutput[p] = float4(n, pixelSize);
        // The sky dome and far horizon keep the game's own shading.
        if (dot(n, n) < 0.5 || length(world - worldPosition(p, 0)) > skyDistance) {
            traceOutput[p] = 0;
            return;
        }
        uint seed = hash(thread.x + thread.y * size.x + hash(frameIndex));
        float2 random = float2(hash(seed), hash(seed ^ 0x9e3779b9)) / 4294967296.0;
        float radius = sqrt(random.x), angle = 6.28318530718 * random.y;
        float3 tangent = normalize(cross(abs(n.z) < 0.9 ? float3(0, 0, 1) : float3(0, 1, 0), n));
        float3 direction = radius * cos(angle) * tangent + radius * sin(angle) * cross(n, tangent)
            + sqrt(1 - random.x) * n;
        RayDesc ray;
        // Also covers sample-0 depth falling below the surface in the fallback.
        ray.Origin = world + n * max(0.0001, pixelSize * 0.5);
        ray.Direction = direction;
        ray.TMin = layerDistance;
        ray.TMax = 1e7;
        // A ray leaving a surface inside an overlapping mesh, such as an arm at the
        // elbow, sees only that mesh's back faces, which must not occlude it.
        RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> query;
        query.TraceRayInline(scene, RAY_FLAG_NONE, 0xff, ray);
        while (query.Proceed()) {
            if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                alphaPass(query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
                query.CommitNonOpaqueTriangleHit();
        }
        // Signed change from open sky, which the game's ambient already lights.
        // The sky dome and cloud cards write depth, so distant hits count as sky.
        if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT && query.CommittedRayT() < skyDistance) {
            float3 hit = ray.Origin + direction * query.CommittedRayT();
            float2 uv;
            float hitDepth;
            bool lit = false;
            if (project(hit, viewProjection, uv, hitDepth)) {
                uint first = 3 * query.CommittedPrimitiveIndex();
                float3 a = positions[indices[first]], b = positions[indices[first + 1]];
                float3 hitNormal = cross(b - a, positions[indices[first + 2]] - a);
                // The screen holds only the camera-facing side; a thin surface's
                // far side would otherwise borrow its lit near side's color.
                bool seen = dot(hitNormal, unproject(uv, 0) - hit) * dot(hitNormal, direction) < 0;
                int2 samplePixel = int2(uv * size);
                if (seen && depthAgrees(hitDepth, depthImage[samplePixel])) {
                    radiance = colorImage[samplePixel].rgb - skyLevel;
                    lit = true;
                }
            }
            // A hidden occluder close by, such as a body over sand, blocks the sky.
            if (!lit) radiance = -skyLevel * saturate(1 - query.CommittedRayT() / contactDistance);
        }
    }
    traceOutput[p] = float4(radiance, d < 1 ? 1 : 0);
}

[numthreads(8, 8, 1)]
void temporalMain(uint3 thread : SV_DispatchThreadID)
{
    int2 p = thread.xy;
    if (any(thread.xy >= size)) return;
    if (depthImage[p] >= 1) { historyOutput[p] = 0; return; }
    float3 world = surfacePosition[p].xyz;
    float4 normal = surfaceNormal[p];
    float3 mean = 0, secondMoment = 0;
    float total = 0;
    // Current-frame statistics also bound history when geometry moves without
    // motion vectors. Exclude other surfaces so their light cannot leak in.
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            int2 q = bounded(p + int2(x, y));
            float4 position = surfacePosition[q], n = surfaceNormal[q];
            float3 delta = position.xyz - world;
            float planeDistance = max(abs(dot(delta, normal.xyz)), abs(dot(delta, n.xyz)));
            float weight = position.w < 1 && dot(normal.xyz, n.xyz) > 0.5 &&
                planeDistance < max(0.0001, 3 * normal.w) ? 1 : 0;
            float3 sample = tracedGI[q].rgb;
            mean += weight * sample;
            secondMoment += weight * sample * sample;
            total += weight;
        }
    }
    mean /= max(total, 1);
    float3 sigma = sqrt(max(0, secondMoment / max(total, 1) - mean * mean));
    float3 low = mean - 2 * sigma, high = mean + 2 * sigma;
    float4 result = tracedGI[p];
    if (historyValid) {
        float2 uv;
        float oldDepth;
        if (project(world, previousViewProjection, uv, oldDepth)) {
            int2 oldPixel = int2(uv * size);
            float4 oldPosition = previousPosition[oldPixel], oldNormal = previousNormal[oldPixel];
            // Depth/normal disagreement is disocclusion. World-position motion
            // alone is not: retain history, bounded by the current neighbourhood.
            if (depthAgrees(oldDepth, oldPosition.w) && dot(normal.xyz, oldNormal.xyz) > 0.8) {
                float4 history = previousGI[oldPixel];
                float count = min(history.a, 31);
                result = float4(lerp(clamp(history.rgb, low, high), result.rgb, 1 / (count + 1)), count + 1);
            }
        }
    }
    result.rgb = clamp(result.rgb, low, high);
    historyOutput[p] = result;
}

[numthreads(8, 8, 1)]
void spatialMain(uint3 thread : SV_DispatchThreadID)
{
    int2 p = thread.xy;
    if (any(thread.xy >= size)) return;
    float4 center = accumulatedGI[p];
    if (depthImage[p] >= 1) { giOutput[p] = 0; return; }
    // The broad fourth pass is for newly exposed or continually moving pixels.
    if (filterStep == 8 && center.a >= 8) { giOutput[p] = center; return; }
    float3 world = surfacePosition[p].xyz;
    float4 n = surfaceNormal[p];
    float3 sum = 0;
    float total = 0;
    float normalPower = center.a < 8 ? 4 : 8;
    float luminanceScale = 0.06 + 0.5 * rsqrt(max(center.a, 1));
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 q = bounded(p + int2(x, y) * int(filterStep));
            float4 otherPosition = surfacePosition[q], otherNormal = surfaceNormal[q];
            float3 delta = otherPosition.xyz - world;
            float planeDistance = max(abs(dot(delta, n.xyz)), abs(dot(delta, otherNormal.xyz)));
            float3 sample = accumulatedGI[q].rgb;
            float weight = (x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)
                * exp(-planeDistance / max(0.0001, n.w * filterStep * 1.5))
                * pow(saturate(dot(n.xyz, otherNormal.xyz)), normalPower)
                * exp(-abs(luminance(sample) - luminance(center.rgb)) / luminanceScale);
            weight *= otherPosition.w < 1;
            sum += sample * weight;
            total += weight;
        }
    }
    giOutput[p] = float4(sum / max(total, 1e-6), center.a);
}
