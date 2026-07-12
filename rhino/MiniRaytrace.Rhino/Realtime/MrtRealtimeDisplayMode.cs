using MiniRaytrace.Rhino.ChangeQueue;
using MiniRaytrace.Rhino.Interop;
using MiniRaytrace.Rhino.Presentation;
using MiniRaytrace.Rhino.Session;
using MiniRaytrace.Rhino.Settings;
using Rhino;
using Rhino.DocObjects;
using Rhino.Render;

namespace MiniRaytrace.Rhino.Realtime;

/// <summary>
/// Viewport realtime display mode (design doc §3/§4, C2). Owns one
/// <see cref="RenderSession"/> per active viewport session; all the actual
/// mrt* calls happen on that session's render thread (never here — this
/// class runs on Rhino's UI thread except where Rhino explicitly documents
/// otherwise).
/// </summary>
public class MrtRealtimeDisplayMode : global::Rhino.Render.RealtimeDisplayMode
{
    private RenderSession? _session;
    private RenderWindow? _renderWindow;
    private MrtChangeQueue? _changeQueue;
    private int _width, _height;
    private bool _started;
    private bool _worldCreated; // render thread only (set before session start)
    private bool _forCapture;
    private uint _captureSppTarget = 128; // design doc E4 default; TODO(E5): configurable
    private DateTime _startTime;

    // Set from the render thread inside OnFrameRendered, read from Rhino's UI
    // thread by the Hud*/IsCompleted methods below — plain fields are fine
    // here (single-writer, torn reads of an int/bool are not a correctness
    // concern for HUD display text).
    private volatile uint _lastSpp;
    private volatile bool _lastDenoised;
    private volatile bool _isCompleted;
    private volatile bool _isInteractive;
    private volatile float _lastFrameMs;
    private DateTime _lastPresentUtc = DateTime.MinValue;
    private int _sppLimit = 1024;

    // HUD pause/lock state. CRITICAL: this class OWNS this state - the base
    // class's Paused/Locked property getters are literally
    // `callvirt HudRendererPaused()` / `callvirt HudRendererLocked()`
    // (verified in the runtime RhinoCommon.dll IL, 8.22 - NOT visible in the
    // NuGet reference assembly, whose method bodies are stubs). An override
    // that returns base.Paused therefore recurses infinitely and
    // stack-overflows the whole Rhino process; that was 0.2.1's crash.
    // The only inputs are the HUD button events wired up in the constructor.
    private volatile bool _hudPaused;
    private volatile bool _hudLocked;

    public MrtRealtimeDisplayMode()
    {
        // Both the old *Pressed and newer *LeftClicked event families are
        // wired: Pressed is marked obsolete in favor of LeftClicked, but
        // which one a given Rhino build actually raises can't be verified
        // without a live session. Handlers are idempotent, so a build that
        // raises both just sets the same state twice.
#pragma warning disable CS0618
        HudPlayButtonPressed += (_, _) => SetHudPaused(false);
        HudPauseButtonPressed += (_, _) => SetHudPaused(true);
        HudLockButtonPressed += (_, _) => _hudLocked = true;
        HudUnlockButtonPressed += (_, _) => _hudLocked = false;
#pragma warning restore CS0618
        HudPlayButtonLeftClicked += (_, _) => SetHudPaused(false);
        HudPauseButtonLeftClicked += (_, _) => SetHudPaused(true);
        HudLockButtonLeftClicked += (_, _) => _hudLocked = true;
        HudUnlockButtonLeftClicked += (_, _) => _hudLocked = false;
    }

    private void SetHudPaused(bool paused)
    {
        _hudPaused = paused;
        if (paused) _session?.Pause();
        else _session?.Resume();
    }

    // Interactive down-res (issue E2): while the camera is moving, the
    // engine renders at 1/2 resolution and FramePresenter upsamples into the
    // (unchanged) RenderWindow size; 200ms after the last camera move it
    // steps back up to full resolution. _activeScale always reflects the
    // resolution the engine actually rendered the LAST delivered frame at -
    // see the ordering note in OnFrameRendered before changing this logic.
    private DateTime _lastViewChangeUtc = DateTime.UtcNow - TimeSpan.FromSeconds(1);
    private int _activeScale = 1;
    private static readonly TimeSpan InteractiveWindow = TimeSpan.FromMilliseconds(200);

    public override bool StartRenderer(int w, int h, RhinoDoc doc, ViewInfo view, ViewportInfo viewportInfo,
        bool forCapture, RenderWindow renderWindow)
    {
        _width = w;
        _height = h;
        _renderWindow = renderWindow;
        _forCapture = forCapture;
        renderWindow.SetSize(new System.Drawing.Size(w, h));
        // UTC: Rhino computes the HUD elapsed time against a UTC clock —
        // local time here made the timer show negative ("00:-58:-20").
        _startTime = DateTime.UtcNow;
        _isCompleted = false;
        _lastSpp = 0;
        _activeScale = 1;

        _changeQueue = new MrtChangeQueue(MrtRenderPlugIn.Instance.Id, doc.RuntimeSerialNumber, view);
        _changeQueue.ViewChanged += () => _lastViewChangeUtc = DateTime.UtcNow;
        // CreateWorld() must NOT be called here: it fires the Apply*
        // callbacks synchronously before returning (ApplyViewChange arrives
        // mid-call), and Engine isn't set yet - the first live-session test
        // crashed all of Rhino exactly this way (unhandled exception escaping
        // a native RDK callback). The render thread does it in SyncScene.
        _worldCreated = false;

        var settingsData = MrtRenderSettingsData.For(doc);
        _sppLimit = (int)settingsData.SppLimit;

        _session = new RenderSession((uint)w, (uint)h,
            syncScene: SyncScene,
            onFrameRendered: OnFrameRendered,
            onFatalError: OnFatalError,
            initialSettings: settingsData.ToEngineSettings());
        _session.Start();
        _started = true;

        // ViewCaptureToFile/print (issue E4): render synchronously to a
        // fixed spp target instead of handing back an interactive session -
        // the caller expects a finished image the moment this returns.
        if (forCapture)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (!_isCompleted && _lastSpp < _captureSppTarget && sw.Elapsed < TimeSpan.FromSeconds(120))
                Thread.Sleep(10);
            _isCompleted = true;
        }

        return true;
    }

    // Runs on the render thread (RenderSession.RenderLoop) every iteration.
    // First call: CreateWorld(), which synchronously delivers the full
    // document through the Apply* callbacks right here on this thread -
    // hence Engine must be assigned first. Every call after that: Flush()
    // delivers whatever changed since the previous one (design doc §4.2/§5).
    private void SyncScene(MrtEngine engine)
    {
        if (_changeQueue is null) return;
        _changeQueue.Engine = engine;
        if (!_worldCreated)
        {
            _worldCreated = true;
            _changeQueue.CreateWorld();
        }
        else
        {
            _changeQueue.Flush();
        }
        // Backfill environment + fallback headlight (a lights-off document
        // must not render pure black - see MrtChangeQueue.PostFlushFixups).
        _changeQueue.PostFlushFixups();
    }

    private void OnFrameRendered(MrtEngine engine, RenderSessionFrame frame)
    {
        _lastSpp = frame.Spp;
        _lastDenoised = frame.Denoised;
        _lastFrameMs = frame.FrameMs;
        _isCompleted = frame.Converged;

        // Pause/resume is driven by the HUD button events (see ctor); this is
        // just a reconciliation fallback for an event that fired before
        // _session existed. NEVER read base.Paused here — its getter calls
        // HudRendererPaused(), i.e. back into this class (see _hudPaused).
        if (_hudPaused && _session is { IsPaused: false }) _session.Pause();
        else if (!_hudPaused && _session is { IsPaused: true }) _session.Resume();

        // Present at the resolution the engine ACTUALLY rendered this frame
        // at (RenderSession.AppliedWidth/Height, maintained on this same
        // thread) - deriving it from _activeScale raced against resizes
        // requested from the UI thread and showed stale-memory garbage in
        // the bottom of the viewport (native readback fills only
        // engineW*engineH pixels of a larger destination).
        int renderW = (int)(_session?.AppliedWidth ?? 0);
        int renderH = (int)(_session?.AppliedHeight ?? 0);
        if (renderW <= 0 || renderH <= 0) { renderW = _width; renderH = _height; }

        // Presentation throttle (design doc §5.5): early frames land every
        // time for instant feedback; once the image is converging, readback
        // + channel writes drop to ~15Hz to stop stealing GPU/CPU time from
        // actual rendering.
        DateTime now = DateTime.UtcNow;
        bool present = frame.Spp < 32 || frame.Converged || (now - _lastPresentUtc).TotalMilliseconds > 66;
        if (present && _renderWindow is not null)
        {
            FramePresenter.Present(engine, _renderWindow, _width, _height, renderW, renderH);
            _lastPresentUtc = now;
        }

        bool interactive = !_forCapture && now - _lastViewChangeUtc < InteractiveWindow;
        _isInteractive = interactive;
        int desiredScale = interactive ? 2 : 1;
        if (desiredScale != _activeScale)
        {
            _activeScale = desiredScale;
            _session?.Resize((uint)Math.Max(1, _width / desiredScale), (uint)Math.Max(1, _height / desiredScale));
        }

        if (present) SignalRedraw();
    }

    private void OnFatalError(Exception ex)
    {
        RhinoApp.WriteLine($"MiniRaytrace: render thread stopped: {ex.Message}");
        _isCompleted = true;
    }

    public override void GetRenderSize(out int width, out int height)
    {
        width = _width;
        height = _height;
    }

    public override int LastRenderedPass() => (int)_lastSpp;

    public override bool IsFrameBufferAvailable(ViewInfo view) => true;

    public override bool IsRendererStarted() => _started;

    public override bool OnRenderSizeChanged(int width, int height)
    {
        _width = width;
        _height = height;
        _activeScale = 1; // re-evaluated against the new size on the next OnFrameRendered
        _session?.Resize((uint)width, (uint)height);
        return true;
    }

    public override void ShutdownRenderer()
    {
        _session?.Shutdown();
        _session = null;
        _changeQueue?.Dispose();
        _changeQueue = null;
        _started = false;
    }

    public override bool IsCompleted() => _isCompleted;

    // ---- HUD (design doc §8 C3) --------------------------------------------
    public override string HudProductName() => "MiniRaytrace";
    public override int HudMaximumPasses() => _sppLimit;
    public override int HudLastRenderedPass() => (int)_lastSpp;
    // MUST return our own state, never base.Paused/Locked — those getters
    // call right back into these methods (infinite recursion; killed Rhino
    // in 0.2.1's live test, WER stack-overflow signature pointed here).
    public override bool HudRendererPaused() => _hudPaused;
    public override bool HudRendererLocked() => _hudLocked;
    // Inline HUD editing isn't wired up; use _MiniRaytraceSettings instead (issue E5).
    public override bool HudAllowEditMaxPasses() => false;
    public override bool HudShowMaxPasses() => true;
    public override bool HudShowPasses() => true;
    public override bool HudShowCustomStatusText() => true;
    public override string HudCustomStatusText()
    {
        // Frame time comes straight from the native engine (GPU render wall
        // time per frame) - doubles as live proof of which device is doing
        // the work and how fast.
        string state = _isInteractive ? "interactive" : _lastDenoised ? "denoised" : "rendering";
        float ms = _lastFrameMs;
        return ms > 0f ? $"{state} · {ms:F0} ms/frame" : state;
    }
    public override DateTime HudStartTime() => _startTime;
}
