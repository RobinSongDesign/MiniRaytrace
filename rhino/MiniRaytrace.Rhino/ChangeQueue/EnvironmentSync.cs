using System.Drawing;
using MiniRaytrace.Rhino.Interop;
using Rhino.Render;
using RCQ = Rhino.Render.ChangeQueue;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Sun is handled in LightSync.cs (it arrives as a Light, not an
/// Environment); this file covers skylight + background/reflection
/// environment (design doc §5.2/§5.6, issue D6).
///
/// Scope cuts, both logged once rather than silently wrong:
///  - v1's single mrtSetEnvironment intensity knob can't separate "shown as
///    background" from "contributes lighting" (design doc: "v1 doesn't
///    distinguish the reflection channel"), so turning Skylight off dims the
///    background too, not just the lighting contribution.
///  - Only LDR bitmap-backed environments (png/jpg/bmp/tiff) are decoded via
///    System.Drawing; true HDR files (.hdr/.exr) and fully procedural
///    environments (physical sky, noise) fall back to their average/solid
///    color instead of a real bake - there's no HDR image codec available
///    here without vendoring a third-party decoder.
/// </summary>
public sealed partial class MrtChangeQueue
{
    private bool _skylightEnabled = true;
    private bool _envEverApplied;

    /// <summary>True when the current environment actually contributes light
    /// (nonzero intensity, non-black content) - drives the fallback headlight
    /// decision in PostFlushFixups.</summary>
    internal bool EnvironmentLit { get; private set; }

    /// <summary>
    /// Called by PostFlushFixups when no skylight/environment event arrived
    /// during CreateWorld - without it the engine has no environment at all
    /// and every miss ray returns pure black (the "switched mode, everything
    /// black" failure from the first render test).
    /// </summary>
    internal void EnsureEnvironmentInitialized()
    {
        if (!_envEverApplied) RefreshEnvironment();
    }

    protected override void ApplySkylightChanges(RCQ.Skylight skylight) =>
        Guarded(nameof(ApplySkylightChanges), () =>
        {
            _skylightEnabled = skylight.Enabled;
            RefreshEnvironment();
        });

    protected override void ApplyEnvironmentChanges(RenderEnvironment.Usage usage) =>
        Guarded(nameof(ApplyEnvironmentChanges), RefreshEnvironment);

    private void RefreshEnvironment()
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");
        _envEverApplied = true;
        float intensity = _skylightEnabled ? 1.0f : 0.0f;

        // The environment that drives skylighting takes priority (that's the
        // one lighting the scene - in default docs the "Studio" skylight env
        // lives under this usage, NOT under Background); fall back to the
        // background environment, then to the plain background color.
        uint crc = _skylightEnabled ? EnvironmentIdForUsage(RenderEnvironment.Usage.Skylighting) : 0;
        if (crc == 0) crc = EnvironmentIdForUsage(RenderEnvironment.Usage.Background);
        RenderEnvironment? env = crc != 0 ? EnvironmentForid(crc) : null;

        if (env is not null)
        {
            SimulatedEnvironment sim = env.SimulateEnvironment(isForDataOnly: true);
            SimulatedTexture? bg = sim.BackgroundImage;

            if (bg is not null && !string.IsNullOrEmpty(bg.Filename) && File.Exists(bg.Filename))
            {
                BakeEquirectFromFile(bg.Filename, (float)bg.Rotation, intensity, engine);
                EnvironmentLit = intensity > 0f;
                return;
            }

            if (bg is not null)
                ChangeQueueLog.WarnOnce("env-procedural",
                    "procedural/physical-sky environments aren't baked in v1, using an average color instead");

            SetSolidEnvironment(sim.BackgroundColor, 0f, intensity, engine);
            EnvironmentLit = intensity > 0f && (sim.BackgroundColor.R + sim.BackgroundColor.G + sim.BackgroundColor.B) > 0;
            return;
        }

        // No environment assigned at all (default docs use a plain background
        // color, which is not an environment object). Upload that color as a
        // 1x1 env anyway: it keeps the background from rendering void-black
        // and, with skylight on, doubles as the ambient dome.
        Color bgColor = GetQueueRenderSettings().BackgroundColorTop;
        SetSolidEnvironment(bgColor, 0f, intensity, engine);
        EnvironmentLit = intensity > 0f && (bgColor.R + bgColor.G + bgColor.B) > 0;
    }

    private const int EnvBakeWidth = 1024, EnvBakeHeight = 512; // design doc §5.2 procedural-bake size, reused here

    private static void BakeEquirectFromFile(string filename, float rotationRad, float intensity, MrtEngine engine)
    {
        string ext = Path.GetExtension(filename).ToLowerInvariant();
        if (ext is ".hdr" or ".exr")
        {
            ChangeQueueLog.WarnOnce("env-hdr-format",
                $"HDR environment files (.hdr/.exr) aren't decoded in v1 ({filename}), using an average color instead");
            SetSolidEnvironment(Color.Gray, rotationRad, intensity, engine);
            return;
        }

        try
        {
            using var bmp = new Bitmap(filename);
            int srcW = bmp.Width, srcH = bmp.Height;
            var pixels = new float[EnvBakeWidth * EnvBakeHeight * 4];

            for (int y = 0; y < EnvBakeHeight; y++)
            {
                int sy = Math.Min(srcH - 1, y * srcH / EnvBakeHeight);
                for (int x = 0; x < EnvBakeWidth; x++)
                {
                    int sx = Math.Min(srcW - 1, x * srcW / EnvBakeWidth);
                    Color px = bmp.GetPixel(sx, sy);
                    int i = (y * EnvBakeWidth + x) * 4;
                    pixels[i + 0] = MathF.Pow(px.R / 255f, 2.2f);
                    pixels[i + 1] = MathF.Pow(px.G / 255f, 2.2f);
                    pixels[i + 2] = MathF.Pow(px.B / 255f, 2.2f);
                    pixels[i + 3] = 1f;
                }
            }

            var desc = MrtTextureDesc.Default;
            desc.Width = EnvBakeWidth;
            desc.Height = EnvBakeHeight;
            desc.Format = MrtTextureFormat.Rgba32f;
            desc.GenerateMips = 0;
            unsafe
            {
                fixed (float* p = pixels)
                {
                    desc.Pixels = (IntPtr)p;
                    engine.SetEnvironment(desc, rotationRad, intensity);
                }
            }
        }
        catch (Exception ex)
        {
            ChangeQueueLog.WarnOnce("env-load-fail", $"failed to load environment image {filename}: {ex.Message}");
            SetSolidEnvironment(Color.Gray, rotationRad, intensity, engine);
        }
    }

    private static void SetSolidEnvironment(Color color, float rotationRad, float intensity, MrtEngine engine)
    {
        var pixel = new[]
        {
            MathF.Pow(color.R / 255f, 2.2f),
            MathF.Pow(color.G / 255f, 2.2f),
            MathF.Pow(color.B / 255f, 2.2f),
            1f,
        };

        var desc = MrtTextureDesc.Default;
        desc.Width = 1;
        desc.Height = 1;
        desc.Format = MrtTextureFormat.Rgba32f;
        desc.GenerateMips = 0;
        unsafe
        {
            fixed (float* p = pixel)
            {
                desc.Pixels = (IntPtr)p;
                engine.SetEnvironment(desc, rotationRad, intensity);
            }
        }
    }
}
