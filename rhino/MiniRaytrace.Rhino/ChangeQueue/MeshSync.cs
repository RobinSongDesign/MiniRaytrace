using System.Runtime.InteropServices;
using Rhino.Geometry;
using RCQ = Rhino.Render.ChangeQueue;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Mesh + instance translation (design doc §5.2 rows 2-3, issue D2). Geometry
/// coordinates pass straight through with zero conversion: the core doesn't
/// care about the up axis, so the whole scene stays in Rhino's own Z-up
/// world coordinates and only the camera/sun directions need to agree with
/// that (design doc §5.2 decision).
/// </summary>
public sealed partial class MrtChangeQueue
{
    protected override void ApplyMeshChanges(Guid[] deleted, List<RCQ.Mesh> added) =>
        Guarded(nameof(ApplyMeshChanges), () => ApplyMeshChangesCore(deleted, added));

    private void ApplyMeshChangesCore(Guid[] deleted, List<RCQ.Mesh> added)
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");

        foreach (Guid meshId in deleted)
        {
            List<int>? indices = null;
            foreach (var key in Handles.Meshes.Keys)
                if (key.MeshId == meshId) (indices ??= new()).Add(key.MeshIndex);

            if (indices is null) continue;
            foreach (int meshIndex in indices)
            {
                engine.RemoveMesh(Handles.Meshes[(meshId, meshIndex)]);
                Handles.Meshes.Remove((meshId, meshIndex));
            }
        }

        foreach (RCQ.Mesh chMesh in added)
        {
            Guid meshId = chMesh.Id();
            Mesh[] subMeshes = chMesh.GetMeshes();

            // MeshInstance.MeshIndex is marked obsolete in this RhinoCommon
            // version ("will always return 0"), so a multi-material object
            // that splits into more than one sub-mesh here can only ever be
            // referenced at index 0 from ApplyMeshInstanceChanges below -
            // materials past the first would silently go unassigned. Flagged
            // rather than silently wrong; needs checking against a live
            // multi-material object (risk RH1 - the ChangeQueue API appears
            // to have moved on from the split-by-material model this file
            // was designed against).
            if (subMeshes.Length > 1)
                ChangeQueueLog.WarnOnce($"multi-submesh-{meshId}",
                    $"object {meshId}: has {subMeshes.Length} per-material sub-meshes, but this Rhino version's MeshInstance.MeshIndex is always 0 - only the first material may render");

            // A material-count decrease can shrink the number of sub-meshes
            // for this Guid; drop the now-unused tail indices.
            var stale = Handles.Meshes.Keys.Where(k => k.MeshId == meshId && k.MeshIndex >= subMeshes.Length).ToList();
            foreach (var key in stale)
            {
                engine.RemoveMesh(Handles.Meshes[key]);
                Handles.Meshes.Remove(key);
            }

            for (int meshIndex = 0; meshIndex < subMeshes.Length; meshIndex++)
            {
                Mesh mesh = subMeshes[meshIndex];
                float[] positions = mesh.Vertices.ToFloatArray();
                uint[] indices = MemoryMarshal.Cast<int, uint>(mesh.Faces.ToIntArray(asTriangles: true)).ToArray();
                float[]? normals = mesh.Normals.Count == mesh.Vertices.Count ? mesh.Normals.ToFloatArray() : null;
                float[]? uvs = mesh.TextureCoordinates.Count == mesh.Vertices.Count ? mesh.TextureCoordinates.ToFloatArray() : null;

                var key = (meshId, meshIndex);
                if (Handles.Meshes.TryGetValue(key, out uint existingId))
                {
                    engine.UpdateMesh(existingId, positions, indices, normals, uvs);
                }
                else
                {
                    uint newId = engine.AddMesh(positions, indices, normals, uvs);
                    Handles.Meshes[key] = newId;
                }
            }
        }
    }

    protected override void ApplyMeshInstanceChanges(List<uint> deleted, List<RCQ.MeshInstance> addedOrChanged) =>
        Guarded(nameof(ApplyMeshInstanceChanges), () => ApplyMeshInstanceChangesCore(deleted, addedOrChanged));

    private void ApplyMeshInstanceChangesCore(List<uint> deleted, List<RCQ.MeshInstance> addedOrChanged)
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");

        foreach (uint instanceId in deleted)
        {
            if (Handles.Instances.Remove(instanceId, out var rec))
                engine.RemoveInstance(rec.MrtInstanceId);
        }

        foreach (RCQ.MeshInstance chInstance in addedOrChanged)
        {
            var meshKey = (chInstance.MeshId, chInstance.MeshIndex);
            if (!Handles.Meshes.TryGetValue(meshKey, out uint mrtMeshId))
            {
                ChangeQueueLog.Warn($"instance {chInstance.InstanceId} references unseen mesh {chInstance.MeshId}/{chInstance.MeshIndex} - skipped");
                continue;
            }

            uint materialCrc = chInstance.MaterialId;
            uint mrtMaterialId = EnsureMaterial(materialCrc, chInstance.RenderMaterial);
            float[] xform3x4 = ToXform3x4(chInstance.Transform);

            if (Handles.Instances.TryGetValue(chInstance.InstanceId, out var existing))
            {
                if (existing.MeshId == meshKey.MeshId && existing.MeshIndex == meshKey.MeshIndex && existing.MaterialCrc == materialCrc)
                {
                    // Pure transform change - the common case (move/copy/gizmo
                    // drag) - stays on the cheap path so it can keep up with
                    // interactive editing (D2 acceptance: <100ms).
                    engine.SetInstanceTransform(existing.MrtInstanceId, xform3x4);
                }
                else
                {
                    // Mesh or material identity changed under the same
                    // instance id. There's no mrtSetInstanceMesh/Material in
                    // the C ABI, so recreate - acceptable since this path is
                    // reassignment, not everyday transform editing.
                    engine.RemoveInstance(existing.MrtInstanceId);
                    uint recreated = engine.AddInstance(mrtMeshId, xform3x4, mrtMaterialId);
                    Handles.Instances[chInstance.InstanceId] = new HandleMaps.InstanceRecord(recreated, meshKey.MeshId, meshKey.MeshIndex, materialCrc);
                }
            }
            else
            {
                uint newId = engine.AddInstance(mrtMeshId, xform3x4, mrtMaterialId);
                Handles.Instances[chInstance.InstanceId] = new HandleMaps.InstanceRecord(newId, meshKey.MeshId, meshKey.MeshIndex, materialCrc);
            }
        }
    }

    private static float[] ToXform3x4(Transform t) => new[]
    {
        (float)t.M00, (float)t.M01, (float)t.M02, (float)t.M03,
        (float)t.M10, (float)t.M11, (float)t.M12, (float)t.M13,
        (float)t.M20, (float)t.M21, (float)t.M22, (float)t.M23,
    };
}
