using MiniRaytrace.Rhino.Interop;
using Rhino.Geometry;
using RCQ = Rhino.Render.ChangeQueue;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>GroundPlane and ClippingPlane handling (design doc §5.2, issue D8).</summary>
public sealed partial class MrtChangeQueue
{
    protected override void ApplyGroundPlaneChanges(RCQ.GroundPlane gp) =>
        Guarded(nameof(ApplyGroundPlaneChanges), () => ApplyGroundPlaneChangesCore(gp));

    private void ApplyGroundPlaneChangesCore(RCQ.GroundPlane gp)
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");

        if (!gp.Enabled)
        {
            RemoveGroundPlane(engine);
            return;
        }

        BoundingBox bbox = GetQueueSceneBoundingBox();
        double size = bbox.IsValid ? bbox.Diagonal.Length * 10.0 : 0.0;
        if (size < 1.0) size = 1000.0; // empty/degenerate scene fallback

        Point3d center = bbox.IsValid ? bbox.Center : Point3d.Origin;
        double z = gp.Altitude;

        float[] positions =
        {
            (float)(center.X - size), (float)(center.Y - size), (float)z,
            (float)(center.X + size), (float)(center.Y - size), (float)z,
            (float)(center.X + size), (float)(center.Y + size), (float)z,
            (float)(center.X - size), (float)(center.Y + size), (float)z,
        };
        uint[] indices = { 0, 1, 2, 0, 2, 3 };

        uint mrtMaterialId = EnsureMaterial(gp.MaterialId, MaterialFromId(gp.MaterialId));

        if (_groundPlaneMeshId == 0)
        {
            _groundPlaneMeshId = engine.AddMesh(positions, indices);
            _groundPlaneInstanceId = engine.AddInstance(_groundPlaneMeshId, xform3x4: null, mrtMaterialId);
            _groundPlaneMaterialCrc = gp.MaterialId;
        }
        else
        {
            // Bounding box growth (new geometry added far away) reshapes the
            // plane in place - same mesh id, no instance recreate needed.
            engine.UpdateMesh(_groundPlaneMeshId, positions, indices);
            if (_groundPlaneMaterialCrc != gp.MaterialId)
            {
                engine.RemoveInstance(_groundPlaneInstanceId);
                _groundPlaneInstanceId = engine.AddInstance(_groundPlaneMeshId, xform3x4: null, mrtMaterialId);
                _groundPlaneMaterialCrc = gp.MaterialId;
            }
        }
    }

    private void RemoveGroundPlane(MrtEngine engine)
    {
        if (_groundPlaneInstanceId != 0) engine.RemoveInstance(_groundPlaneInstanceId);
        if (_groundPlaneMeshId != 0) engine.RemoveMesh(_groundPlaneMeshId);
        _groundPlaneInstanceId = 0;
        _groundPlaneMeshId = 0;
        _groundPlaneMaterialCrc = 0;
    }

    protected override void ApplyClippingPlaneChanges(Guid[] deleted, List<RCQ.ClippingPlane> addedOrModified)
    {
        if (addedOrModified.Count > 0)
            ChangeQueueLog.WarnOnce("clipping-planes",
                "clipping planes aren't supported yet (v2: shader-side plane clipping) - this document has one and it will render as if it weren't there");
    }
}
