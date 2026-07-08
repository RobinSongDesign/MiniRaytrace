using System.Drawing;
using MiniRaytrace.Rhino.ChangeQueue;
using MiniRaytrace.Rhino.Interop;
using MiniRaytrace.Rhino.Session;
using MiniRaytrace.Rhino.Settings;
using Rhino;
using Rhino.Commands;
using Rhino.Display;
using Rhino.DocObjects;
using Rhino.PlugIns;
using Rhino.Render;

namespace MiniRaytrace.Rhino.Presentation;

/// <summary>
/// Modal production-render pipeline behind <c>_Render</c> (design doc §7,
/// issue C1). Drives one off-screen <see cref="RenderSession"/> exactly like
/// the realtime viewport mode does; the only differences are the resolution
/// source (document render settings, not viewport size) and the presentation
/// target (the modal <see cref="RenderWindow"/> Rhino creates for us).
///
/// Unlike the realtime mode, this only syncs the scene once (a single
/// CreateWorld on the render thread): modal render doesn't track incremental
/// document edits mid-render (design doc §7).
/// </summary>
public sealed class MrtRenderPipeline : RenderPipeline
{
    private readonly RhinoDoc _doc;
    private readonly int _width, _height;
    private RenderSession? _session;
    private global::Rhino.Render.RenderWindow? _renderWindow;
    private MrtChangeQueue? _changeQueue;
    private bool _seeded;
    private volatile bool _finished;
    private uint _sppLimit = 1024;

    public MrtRenderPipeline(RhinoDoc doc, RunMode mode, Size size)
        : base(doc, mode, MrtRenderPlugIn.Instance, size, "MiniRaytrace",
               global::Rhino.Render.RenderWindow.StandardChannels.RGBA, reuseRenderWindow: true, clearLastRendering: true)
    {
        _doc = doc;
        _width = size.Width;
        _height = size.Height;
    }

    protected override bool OnRenderBegin()
    {
        _renderWindow = GetRenderWindow();
        if (_renderWindow is null) return false;

        var view = new ViewInfo(_doc.Views.ActiveView.ActiveViewport);
        _changeQueue = new MrtChangeQueue(MrtRenderPlugIn.Instance.Id, _doc.RuntimeSerialNumber, view);
        // CreateWorld() is deliberately NOT called here - it fires the Apply*
        // callbacks synchronously on the calling thread, and Engine isn't set
        // yet (same UI-thread crash the realtime mode hit in its first live
        // test). SyncScene does it on the render thread instead.

        var settingsData = MrtRenderSettingsData.For(_doc);
        _sppLimit = settingsData.SppLimit;

        _finished = false;
        _seeded = false;
        _session = new RenderSession((uint)_width, (uint)_height,
            syncScene: SyncScene,
            onFrameRendered: OnFrameRendered,
            onFatalError: OnFatalError,
            initialSettings: settingsData.ToEngineSettings());
        _session.Start();
        return true;
    }

    protected override bool OnRenderWindowBegin(RhinoView view, Rectangle rectangle) => true;

    protected override bool ContinueModal() => !_finished;

    protected override void OnRenderEnd(RenderEndEventArgs e)
    {
        _session?.Shutdown();
        _session = null;
        _changeQueue?.Dispose();
        _changeQueue = null;
    }

    private void SyncScene(MrtEngine engine)
    {
        if (_seeded || _changeQueue is null) return;
        _seeded = true;

        // CreateWorld() synchronously delivers the whole document through
        // the Apply* callbacks on THIS thread - exactly what a modal render
        // wants (one full load, no incremental tracking, design doc §7).
        _changeQueue.Engine = engine;
        _changeQueue.CreateWorld();
    }

    private void OnFrameRendered(MrtEngine engine, RenderSessionFrame frame)
    {
        if (_renderWindow is not null)
        {
            FramePresenter.Present(engine, _renderWindow, _width, _height);
            float progress = _sppLimit == 0 ? 1f : Math.Clamp((float)frame.Spp / _sppLimit, 0f, 1f);
            _renderWindow.SetProgress($"MiniRaytrace: {frame.Spp}/{_sppLimit} spp", progress);
        }

        if (frame.Converged) _finished = true;
    }

    private void OnFatalError(Exception ex)
    {
        RhinoApp.WriteLine($"MiniRaytrace: _Render stopped: {ex.Message}");
        _finished = true;
    }
}
