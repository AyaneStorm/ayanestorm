/**
 * asrSceneSkinAudit.glsl
 * Optional GPU position-fidelity audit. Visibility auditing alone cannot detect
 * a surface deformed by output quantization. Reuse the existing counts readback.
 * Author: chanayane@firestorm
 */
layout(buffer_reference, std430) buffer SkinAuditRef
{
    uint samples;
    uint errors;
    uint maximumError;
};

void auditSkinCluster(ViewRef view, Binding binding, Cluster source, uint64_t positions)
{
    if (view.skinAudit == 0u || source.triangles == 0u) return;
    // One vertex per cluster keeps validation affordable; all clusters, including
    // hidden ones, are checked after the vertex pass's compute barrier.
    const uint vertex = clusterIndex(source.indices, 0u);
    const vec3 expected = skinnedPoint(view, binding, vertex);
    const vec3 stored = packedPosition(positions, vertex);
    float error = length(stored - expected);
    if (any(isnan(stored)) || any(isinf(stored)) || isnan(error) || isinf(error)) error = 3.402823e38;
    SkinAuditRef audit = SkinAuditRef(push.output_counts + 836ul);
    atomicAdd(audit.samples, 1u);
    atomicMax(audit.maximumError, floatBitsToUint(error));
    if (error > max(2e-6, length(expected) * 1e-6)) atomicAdd(audit.errors, 1u);
}
