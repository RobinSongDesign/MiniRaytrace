using MiniRaytrace.Rhino.Settings;
using Rhino;
using Rhino.Commands;
using Rhino.Input;

namespace MiniRaytrace.Rhino;

/// <summary>
/// `_MiniRaytraceSettings` - command-line prompt flow for spp limit / max
/// bounces / denoise / exposure (design doc E5 "渲染设置自定义面板"). A full
/// Eto docked panel would need visual verification against a live Rhino
/// session that isn't available here, so this is the command-line
/// equivalent: same settings object, same persistence (WriteDocument/
/// ReadDocument in <see cref="MrtRenderPlugIn"/>), just prompted instead of
/// a docked UI. Swapping in a real panel later only touches this file.
///
/// Takes effect on the next _Render or realtime-mode restart; an already-
/// running realtime viewport session picks up the new settings the next
/// time it resizes or is re-entered (no live-push registry in v1).
/// </summary>
public class MiniRaytraceSettingsCommand : Command
{
    public override string EnglishName => "MiniRaytraceSettings";

    public override Guid Id { get; } = new("6C6A9E4E-2B6C-4B3E-9C7D-6B7B1B9C6B1F");

    protected override Result RunCommand(RhinoDoc doc, RunMode mode)
    {
        var data = MrtRenderSettingsData.For(doc);

        int spp = (int)data.SppLimit;
        Result rc = RhinoGet.GetInteger("Samples per pixel limit", true, ref spp, 1, 1_000_000);
        if (rc != Result.Success) return rc;

        int bounces = (int)data.MaxBounces;
        rc = RhinoGet.GetInteger("Max bounces", true, ref bounces, 0, 64);
        if (rc != Result.Success) return rc;

        bool denoise = data.Denoise;
        rc = RhinoGet.GetBool("Denoise", true, "Off", "On", ref denoise);
        if (rc != Result.Success) return rc;

        double exposure = data.ExposureEV;
        rc = RhinoGet.GetNumber("Exposure (EV)", true, ref exposure, -10.0, 10.0);
        if (rc != Result.Success) return rc;

        data.SppLimit = (uint)spp;
        data.MaxBounces = (uint)bounces;
        data.Denoise = denoise;
        data.ExposureEV = (float)exposure;
        doc.Modified = true;

        RhinoApp.WriteLine($"MiniRaytrace: spp={spp} bounces={bounces} denoise={denoise} exposure={exposure:F1}EV");
        return Result.Success;
    }
}
