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
        float intensity = _skylightEnabled ? 1.0f : 0.0f;

        uint crc = EnvironmentIdForUsage(RenderEnvironment.Usage.Background);
        RenderEnvironment? env = crc != 0 ? EnvironmentForid(crc) : null;
        if (env is null)
        {
            engine.ClearEnvironment();
            return;
        }

        SimulatedEnvironment sim = env.SimulateEnvironment(isForDataOnly: true);
        SimulatedTexture? bg = sim.BackgroundImage;

        if (bg is not null && !string.IsNullOrEmpty(bg.Filename) && File.Exists(bg.Filename))
        {
            BakeEquirectFromFile(bg.Filename, (float)bg.Rotation, intensity, engine);
            return;
        }

        if (bg is not null)
            ChangeQueueLog.WarnOnce("env-procedural",
                "procedural/physical-sky environments aren't baked in v1, using an average color instead");

        SetSolidEnvironment(sim.BackgroundColor, 0f, intensity, engine);
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
