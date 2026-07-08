using MiniRaytrace.Rhino.Interop;
using Rhino.Render;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Texture baking (design doc §5.3, issue D4): bitmap and procedural
/// textures are both baked through <see cref="TextureEvaluator"/> onto a
/// fixed-size grid - one code path for both, at the cost of some waste for
/// plain bitmaps. Baking runs synchronously on the render thread the first
/// time a texture's crc is seen (RenderSession.syncScene -&gt; Flush()), which
/// stalls that frame; deferring this to a background pre-bake is future work
/// (not tracked as its own issue - fold into E2/E5 if it becomes a problem).
/// </summary>
public sealed partial class MrtChangeQueue
{
    private const int BakeSize = 1024; // design doc §5.3 default; configurable later (E5)

    private void BakeMaterialTextures(RenderMaterial content, ref MrtMaterialDesc desc, uint materialCrc)
    {
        desc.BaseColorTex = TryBakeSlot(content, RenderMaterial.StandardChildSlots.PbrBaseColor, materialCrc);
        desc.RoughnessTex = TryBakeSlot(content, RenderMaterial.StandardChildSlots.PbrRoughness, materialCrc);
        desc.MetallicTex = TryBakeSlot(content, RenderMaterial.StandardChildSlots.PbrMetallic, materialCrc);
        desc.EmissionTex = TryBakeSlot(content, RenderMaterial.StandardChildSlots.PbrEmission, materialCrc);

        // v1 only understands true normal maps in the Bump slot; a legacy
        // height-based bump texture in the same slot is dropped with a
        // one-time warning (design doc §5.3 decision, recorded in D4).
        if (content.GetTextureOnFromUsage(RenderMaterial.StandardChildSlots.Bump)
            && content.GetTextureFromUsage(RenderMaterial.StandardChildSlots.Bump) is { } bump)
        {
            if (bump.IsNormalMap())
                desc.NormalTex = EnsureTexture(bump, materialCrc, isNormalMap: true);
            else
                ChangeQueueLog.WarnOnce($"bump-{materialCrc}",
                    $"material {materialCrc}: bump maps aren't supported (only normal maps), ignored");
        }
    }

    private uint TryBakeSlot(RenderMaterial content, RenderMaterial.StandardChildSlots slot, uint materialCrc)
    {
        if (!content.GetTextureOnFromUsage(slot)) return 0;
        RenderTexture? tex = content.GetTextureFromUsage(slot);
        return tex is null ? 0u : EnsureTexture(tex, materialCrc, isNormalMap: false);
    }

    /// <summary>Dedup by RenderHash (design doc §5.1); bakes and caches on first sight.</summary>
    private uint EnsureTexture(RenderTexture tex, uint ownerCrc, bool isNormalMap)
    {
        uint crc = tex.RenderHashWithoutLocalMapping;
        if (Handles.Textures.TryGetValue(crc, out uint existing)) return existing;

        if (tex.GetProjectionMode() != TextureProjectionMode.MappingChannel)
        {
            ChangeQueueLog.WarnOnce($"tex-proj-{crc}",
                $"material {ownerCrc}: non-UV texture mapping (WCS/OCS/box/screen) isn't supported, using a constant color instead");
            return 0;
        }

        using TextureEvaluator? eval = tex.CreateEvaluator(RenderTexture.TextureEvaluatorFlags.Normal);
        if (eval is null)
        {
            ChangeQueueLog.WarnOnce($"tex-eval-{crc}",
                $"material {ownerCrc}: texture has no evaluator, using a constant color instead");
            return 0;
        }

        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");
        bool linear = isNormalMap || tex.IsLinear();

        var texDesc = MrtTextureDesc.Default;
        texDesc.Width = BakeSize;
        texDesc.Height = BakeSize;
        texDesc.GenerateMips = 1;

        uint newId;
        unsafe
        {
            if (tex.IsHdrCapable())
            {
                float[] pixels = eval.WriteToFloatArray2(BakeSize, BakeSize).ToArray();
                texDesc.Format = MrtTextureFormat.Rgba32f;
                fixed (float* p = pixels)
                {
                    texDesc.Pixels = (IntPtr)p;
                    newId = engine.AddTexture(texDesc);
                }
            }
            else
            {
                byte[] pixels = eval.WriteToByteArray2(BakeSize, BakeSize).ToArray();
                texDesc.Format = linear ? MrtTextureFormat.Rgba8Unorm : MrtTextureFormat.Rgba8Srgb;
                fixed (byte* p = pixels)
                {
                    texDesc.Pixels = (IntPtr)p;
                    newId = engine.AddTexture(texDesc);
                }
            }
        }

        Handles.Textures[crc] = newId;
        return newId;
    }
}
