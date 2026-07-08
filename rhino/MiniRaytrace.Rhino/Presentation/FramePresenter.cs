using System.Drawing;
using System.Runtime.InteropServices;
using MiniRaytrace.Rhino.Interop;
using Rhino.Display;
using Rhino.Render;

namespace MiniRaytrace.Rhino.Presentation;

/// <summary>
/// Reads the engine's linear framebuffer and writes it into a Rhino
/// RenderWindow (PRD §9, design doc §5.5 / §8 E1). Shared by the realtime
/// display mode (C2) and modal production render (C1).
///
/// Uses RenderWindow.SetRGBAChannelColors(Size, Color4f[]) — a bulk write —
/// rather than the OpenChannel()/Channel.SetValue(x,y,color) per-pixel loop
/// McNeel's MockingBird sample uses, since millions of individual calls per
/// frame is not viable at interactive rates.
///
/// UNVERIFIED: the array's expected row order for SetRGBAChannelColors could
/// not be confirmed against a running Rhino instance in this environment
/// (no interactive Rhino session available). This assumes row-major,
/// top-down (index = y*width+x, row 0 = top), matching mrtReadFramebuffer's
/// own documented layout and .NET's System.Drawing/Bitmap convention. If the
/// image comes out vertically flipped in Rhino, flip the row iteration order
/// here — everything else in the pipeline is unaffected.
/// </summary>
public static class FramePresenter
{
    // Reused across calls so steady-state rendering doesn't allocate per
    // frame (mirrors the native side's own persistent-scratch-buffer rule,
    // PRD §8 A7).
    private static float[] _linearScratch = Array.Empty<float>();
    private static Color4f[] _colorScratch = Array.Empty<Color4f>();

    public static void Present(MrtEngine engine, RenderWindow renderWindow, int width, int height) =>
        Present(engine, renderWindow, width, height, width, height);

    /// <summary>
    /// Interactive down-res variant (issue E2): the engine may be rendering
    /// at a smaller <paramref name="renderWidth"/>/<paramref name="renderHeight"/>
    /// than the RenderWindow's fixed <paramref name="displayWidth"/>/
    /// <paramref name="displayHeight"/> - nearest-neighbor magnified up to
    /// display size so the RenderWindow itself never resizes mid-drag
    /// (design doc E2: "RenderWindow size unchanged, pixels magnified").
    /// </summary>
    public static void Present(MrtEngine engine, RenderWindow renderWindow,
        int displayWidth, int displayHeight, int renderWidth, int renderHeight)
    {
        int renderCount = renderWidth * renderHeight;
        if (_linearScratch.Length < renderCount * 4)
            _linearScratch = new float[renderCount * 4];

        Span<byte> asBytes = MemoryMarshal.AsBytes(_linearScratch.AsSpan(0, renderCount * 4));
        engine.ReadFramebuffer(MrtPixelFormat.Rgba32fLinear, asBytes);

        int displayCount = displayWidth * displayHeight;
        if (_colorScratch.Length < displayCount)
            _colorScratch = new Color4f[displayCount];

        if (renderWidth == displayWidth && renderHeight == displayHeight)
        {
            for (int i = 0; i < displayCount; i++)
            {
                int b = i * 4;
                _colorScratch[i] = new Color4f(_linearScratch[b], _linearScratch[b + 1], _linearScratch[b + 2], _linearScratch[b + 3]);
            }
        }
        else
        {
            for (int y = 0; y < displayHeight; y++)
            {
                int sy = Math.Min(renderHeight - 1, y * renderHeight / displayHeight);
                for (int x = 0; x < displayWidth; x++)
                {
                    int sx = Math.Min(renderWidth - 1, x * renderWidth / displayWidth);
                    int b = (sy * renderWidth + sx) * 4;
                    _colorScratch[y * displayWidth + x] =
                        new Color4f(_linearScratch[b], _linearScratch[b + 1], _linearScratch[b + 2], _linearScratch[b + 3]);
                }
            }
        }

        renderWindow.SetRGBAChannelColors(new Size(displayWidth, displayHeight),
            displayCount == _colorScratch.Length ? _colorScratch : _colorScratch[..displayCount]);
    }
}
