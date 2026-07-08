using MiniRaytrace.Rhino.Interop;
using Rhino;
using Rhino.Collections;

namespace MiniRaytrace.Rhino.Settings;

/// <summary>
/// Render settings a user can tune (spp limit / max bounces / denoise /
/// exposure), persisted with the 3dm via <see cref="MrtRenderPlugIn"/>'s
/// WriteDocument/ReadDocument (issue E5). One instance per open document.
/// </summary>
public sealed class MrtRenderSettingsData
{
    public uint SppLimit { get; set; } = 1024;
    public uint MaxBounces { get; set; } = 8;
    public bool Denoise { get; set; } = true;
    public float ExposureEV { get; set; }

    private static readonly Dictionary<uint, MrtRenderSettingsData> ByDocSerial = new();

    public static MrtRenderSettingsData For(RhinoDoc doc)
    {
        if (!ByDocSerial.TryGetValue(doc.RuntimeSerialNumber, out var data))
            ByDocSerial[doc.RuntimeSerialNumber] = data = new MrtRenderSettingsData();
        return data;
    }

    public static void Forget(RhinoDoc doc) => ByDocSerial.Remove(doc.RuntimeSerialNumber);

    public void CopyFrom(MrtRenderSettingsData other)
    {
        SppLimit = other.SppLimit;
        MaxBounces = other.MaxBounces;
        Denoise = other.Denoise;
        ExposureEV = other.ExposureEV;
    }

    public MrtRenderSettings ToEngineSettings()
    {
        var settings = MrtRenderSettings.Default;
        settings.SppLimit = SppLimit;
        settings.MaxBounces = MaxBounces;
        settings.Denoise = Denoise ? 1 : 0;
        settings.ExposureEV = ExposureEV;
        return settings;
    }

    private const string SppKey = "spp_limit", BouncesKey = "max_bounces", DenoiseKey = "denoise", ExposureKey = "exposure_ev";

    public ArchivableDictionary ToArchivableDictionary()
    {
        var dict = new ArchivableDictionary();
        dict.Set(SppKey, (int)SppLimit);
        dict.Set(BouncesKey, (int)MaxBounces);
        dict.Set(DenoiseKey, Denoise);
        dict.Set(ExposureKey, ExposureEV);
        return dict;
    }

    public static MrtRenderSettingsData FromArchivableDictionary(ArchivableDictionary dict)
    {
        var data = new MrtRenderSettingsData();
        if (dict.TryGetInteger(SppKey, out int spp)) data.SppLimit = (uint)Math.Max(1, spp);
        if (dict.TryGetInteger(BouncesKey, out int bounces)) data.MaxBounces = (uint)Math.Max(0, bounces);
        if (dict.TryGetBool(DenoiseKey, out bool denoise)) data.Denoise = denoise;
        if (dict.TryGetDouble(ExposureKey, out double exposure)) data.ExposureEV = (float)exposure;
        return data;
    }
}
