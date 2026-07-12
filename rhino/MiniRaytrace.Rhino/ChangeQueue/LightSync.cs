using MiniRaytrace.Rhino.Interop;
using Rhino.Geometry;
using RCQ = Rhino.Render.ChangeQueue;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Light translation and unit conversion (design doc §5.2 light rows, issue
/// D5). Radiometric conversion from Rhino's Watts/Intensity to the core's
/// radiance is a best-effort approximation - the design doc calls for
/// calibrating it against RhinoCycles' own coefficients, which isn't
/// possible without a live side-by-side render (risk RH1); treat the numbers
/// here as a starting point to re-tune once that comparison can be done.
/// </summary>
public sealed partial class MrtChangeQueue
{
    // Unlike ApplyLightChanges, this takes the raw Rhino.Geometry.Light
    // directly (not the RCQ.Light wrapper) - there's exactly one document
    // Sun, no add/delete/crc bookkeeping needed.
    protected override void ApplySunChanges(Light sun) =>
        Guarded(nameof(ApplySunChanges), () =>
        {
            if (!sun.IsEnabled)
            {
                var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");
                if (Handles.Lights.Remove(sun.Id, out uint mrtId)) engine.RemoveLight(mrtId);
                return;
            }

            var desc = MrtLightDesc.Default;
            desc.Type = MrtLightType.Sun;

            // Rhino's Light.Direction is the direction the light TRAVELS
            // (sun -> ground, pointing down); the core wants the direction
            // TOWARD the light (mini_raytrace.h: "sun: towards the light").
            Vector3d dir = sun.Direction;
            dir.Unitize();
            desc.Direction = ToVec3(-dir);
            // Physical sun disc, matches MrtLightDesc.Default's angularRadius.
            desc.Radiance = SunConeRadiance(sun.Diffuse, sun.Intensity, desc.AngularRadius);

            UpsertLight(sun.Id, desc);
        });

    protected override void ApplyLightChanges(List<RCQ.Light> lightChanges) =>
        Guarded(nameof(ApplyLightChanges), () => ApplyLightChangesCore(lightChanges));

    private void ApplyLightChangesCore(List<RCQ.Light> lightChanges)
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");

        foreach (RCQ.Light chLight in lightChanges)
        {
            if (chLight.ChangeType == RCQ.Light.Event.Deleted)
            {
                if (Handles.Lights.Remove(chLight.Id, out uint mrtId)) engine.RemoveLight(mrtId);
                continue;
            }

            Light data = chLight.Data;
            if (!data.IsEnabled)
            {
                if (Handles.Lights.Remove(chLight.Id, out uint mrtId)) engine.RemoveLight(mrtId);
                continue;
            }

            UpsertLight(chLight.Id, TranslateLight(data, chLight.Id));
        }
    }

    private void UpsertLight(Guid id, MrtLightDesc desc)
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");
        if (Handles.Lights.TryGetValue(id, out uint existing))
            engine.UpdateLight(existing, desc);
        else
            Handles.Lights[id] = engine.AddLight(desc);
    }

    private static MrtLightDesc TranslateLight(Light data, Guid id)
    {
        var desc = MrtLightDesc.Default;

        if (data.IsRectangularLight)
        {
            desc.Type = MrtLightType.Rect;
            FillRect(data, ref desc, data.Length, data.Width);
        }
        else if (data.IsLinearLight)
        {
            ChangeQueueLog.WarnOnce($"light-linear-{id}", $"light {id}: linear lights are downgraded to a thin rectangle light");
            desc.Type = MrtLightType.Rect;
            Vector3d edge0 = data.Length;
            Vector3d perp = Vector3d.CrossProduct(edge0, data.Direction);
            if (!perp.Unitize()) perp = Vector3d.XAxis;
            FillRect(data, ref desc, edge0, perp * (edge0.Length * 0.01));
        }
        else if (data.IsSpotLight)
        {
            ChangeQueueLog.WarnOnce($"light-spot-{id}", $"light {id}: spot lights are downgraded to a point light, cone angle ignored");
            desc.Type = MrtLightType.Point;
            desc.Position = ToVec3(data.Location);
            desc.Radiance = PointRadiance(data);
        }
        else if (data.IsPointLight)
        {
            desc.Type = MrtLightType.Point;
            desc.Position = ToVec3(data.Location);
            desc.Radiance = PointRadiance(data);
        }
        else
        {
            // A non-physical "directional light" object, distinct from the
            // document Sun (which comes through ApplySunChanges instead) -
            // approximate as a tight-beam sun. Direction negated: Rhino gives
            // the travel direction, the core wants "towards the light".
            desc.Type = MrtLightType.Sun;
            Vector3d dir = data.Direction;
            dir.Unitize();
            desc.Direction = ToVec3(-dir);
            desc.AngularRadius = 0.0005f;
            desc.Radiance = SunConeRadiance(data.Diffuse, data.Intensity, desc.AngularRadius);
        }

        return desc;
    }

    private static void FillRect(Light data, ref MrtLightDesc desc, Vector3d edge0, Vector3d edge1)
    {
        desc.Corner = ToVec3(data.Location);
        desc.Edge0 = ToVec3(edge0);
        desc.Edge1 = ToVec3(edge1);
        desc.TwoSided = 0;

        double area = edge0.Length * edge1.Length;
        double radiance = area > 1e-9 ? data.PowerWatts * data.Intensity / (area * Math.PI) : 0.0;
        desc.Radiance = ScaleColor(data.Diffuse, radiance);
    }

    private static MrtVec3 PointRadiance(Light data) =>
        ScaleColor(data.Diffuse, data.PowerWatts * data.Intensity / (4.0 * Math.PI));

    /// <summary>
    /// Converts a target on-axis irradiance into the cone radiance the shader
    /// integrates: contribution ≈ L·Ω, Ω = 2π(1-cos r), so L = E/Ω. Feeding
    /// Rhino's Intensity (≈1) in directly as L makes the sun ~4 orders of
    /// magnitude too dim (Ω of the physical sun disc is ~6.8e-5 sr) - that
    /// was a major part of the all-black first render. E = 4·Intensity is an
    /// eyeballed starting calibration; refine against Raytraced side by side
    /// (design doc D5 acceptance).
    /// </summary>
    internal static MrtVec3 SunConeRadiance(System.Drawing.Color diffuse, double intensity, float angularRadius)
    {
        double omega = 2.0 * Math.PI * (1.0 - Math.Cos(angularRadius));
        double e = 4.0 * intensity;
        return ScaleColor(diffuse, e / Math.Max(omega, 1e-9));
    }

    private static MrtVec3 ScaleColor(System.Drawing.Color c, double scale) => new(
        (float)(c.R / 255.0 * scale),
        (float)(c.G / 255.0 * scale),
        (float)(c.B / 255.0 * scale));
}
