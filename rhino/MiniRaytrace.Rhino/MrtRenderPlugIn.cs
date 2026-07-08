using Rhino;
using Rhino.Commands;
using Rhino.FileIO;
using Rhino.PlugIns;
using Rhino.Render;
using MiniRaytrace.Rhino.Presentation;
using MiniRaytrace.Rhino.Settings;

namespace MiniRaytrace.Rhino;

/// <summary>
/// Plug-in entry point. Registers MiniRaytrace as a selectable renderer
/// (Render menu > Current Renderer > MiniRaytrace, PRD §8/§9 design doc §3).
/// The realtime viewport display mode (design doc §3 C2) registers itself
/// automatically from this same assembly — Rhino discovers any
/// RealtimeDisplayMode + RealtimeDisplayModeClassInfo pair on load, no
/// explicit call needed here (confirmed against McNeel's MockingBird sample).
/// </summary>
public class MrtRenderPlugIn : RenderPlugIn
{
    public MrtRenderPlugIn()
    {
        Instance = this;
    }

    public static MrtRenderPlugIn Instance { get; private set; } = null!;

    private const string SettingsChunkName = "MiniRaytraceRenderSettings";

    protected override LoadReturnCode OnLoad(ref string errorMessage)
    {
        RhinoDoc.CloseDocument += (_, e) => MrtRenderSettingsData.Forget(e.Document);
        return LoadReturnCode.Success;
    }

    // Persists MrtRenderSettingsData with the 3dm file (issue E5). Rhino
    // calls these on every registered plug-in during save/open automatically
    // - no explicit registration needed beyond overriding them.
    protected override void WriteDocument(RhinoDoc doc, BinaryArchiveWriter archive, FileWriteOptions options)
    {
        archive.Write3dmChunkVersion(1, 0);
        archive.WriteDictionary(MrtRenderSettingsData.For(doc).ToArchivableDictionary());
    }

    protected override void ReadDocument(RhinoDoc doc, BinaryArchiveReader archive, FileReadOptions options)
    {
        archive.Read3dmChunkVersion(out _, out _);
        var data = MrtRenderSettingsData.FromArchivableDictionary(archive.ReadDictionary());
        MrtRenderSettingsData.For(doc).CopyFrom(data);
    }

    protected override Result Render(RhinoDoc doc, RunMode mode, bool fastPreview)
    {
        var size = doc.RenderSettings.ImageSize;
        if (size.Width <= 0 || size.Height <= 0)
            size = new System.Drawing.Size(800, 600);

        using var pipeline = new MrtRenderPipeline(doc, mode, size);
        RenderPipeline.RenderReturnCode rc = pipeline.Render();
        return rc switch
        {
            RenderPipeline.RenderReturnCode.Ok => Result.Success,
            RenderPipeline.RenderReturnCode.Cancel => Result.Cancel,
            _ => Result.Failure,
        };
    }
}
