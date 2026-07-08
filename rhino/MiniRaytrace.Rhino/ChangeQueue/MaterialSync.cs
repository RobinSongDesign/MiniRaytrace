using MiniRaytrace.Rhino.Interop;
using Rhino.DocObjects;
using Rhino.Render;
using RCQ = Rhino.Render.ChangeQueue;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Material translation (design doc §5.3, issue D3): Rhino PBR -&gt;
/// Disney-lite <see cref="MrtMaterialDesc"/>. Every <see cref="RenderMaterial"/>,
/// PBR-authored or not, goes through <c>ToMaterial</c> -&gt; the legacy
/// <see cref="Material"/> representation -&gt; <c>Material.ToPhysicallyBased()</c>
/// (Rhino's own legacy-to-PBR approximation) - one code path for both, which
/// also happens to be where clearcoat/sheen/anisotropic/subsurface naturally
/// fall away (design doc: "discarded fields, logged once").
/// </summary>
public sealed partial class MrtChangeQueue
{
    protected override void ApplyMaterialChanges(List<RCQ.Material> mats) =>
        Guarded(nameof(ApplyMaterialChanges), () =>
        {
            foreach (RCQ.Material m in mats)
                EnsureMaterial(m.Id, MaterialFromId(m.Id), forceRefresh: true);
        });

    /// <summary>Dedup by content crc (design doc §5.1); returns the mrtMaterialId, creating or refreshing it as needed.</summary>
    internal uint EnsureMaterial(uint crc, RenderMaterial? content, bool forceRefresh = false)
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");

        MrtMaterialDesc desc;
        if (Handles.Materials.TryGetValue(crc, out uint existing))
        {
            if (!forceRefresh) return existing;
            desc = Translate(content, crc);
            engine.UpdateMaterial(existing, desc);
            return existing;
        }

        desc = Translate(content, crc);
        uint newId = engine.AddMaterial(desc);
        Handles.Materials[crc] = newId;
        return newId;
    }

    private MrtMaterialDesc Translate(RenderMaterial? content, uint crc)
    {
        var desc = MrtMaterialDesc.Default;
        if (content is null) return desc; // Rhino always assigns *some* material; defensive only.

        Material legacy = content.ToMaterial(RenderTexture.TextureGeneration.Allow);
        if (!legacy.IsPhysicallyBased) legacy.ToPhysicallyBased();
        global::Rhino.DocObjects.PhysicallyBasedMaterial pbr = legacy.PhysicallyBased;

        if (pbr.Clearcoat > 0 || pbr.Sheen > 0 || pbr.Anisotropic > 0 || pbr.Subsurface > 0)
            ChangeQueueLog.WarnOnce($"mat-drop-{crc}",
                $"material {crc}: clearcoat/sheen/anisotropic/subsurface aren't supported and were dropped");

        desc.BaseColor = new MrtVec3(pbr.BaseColor.R, pbr.BaseColor.G, pbr.BaseColor.B);
        desc.Opacity = (float)pbr.Opacity;
        desc.Roughness = (float)pbr.Roughness;
        desc.Metallic = (float)pbr.Metallic;
        desc.Transmission = (float)(1.0 - pbr.Opacity);
        desc.Ior = (float)(pbr.Opacity < 1.0 ? pbr.OpacityIOR : pbr.ReflectiveIOR);
        desc.Emission = new MrtVec3(pbr.Emission.R, pbr.Emission.G, pbr.Emission.B);

        BakeMaterialTextures(content, ref desc, crc);
        return desc;
    }
}
