using MiniRaytrace.Rhino.Interop;

namespace MiniRaytrace.Rhino.Session;

/// <summary>Snapshot handed to the caller after each rendered frame.</summary>
public readonly record struct RenderSessionFrame(uint Spp, bool Converged, bool Denoised, float FrameMs);

/// <summary>
/// Owns one mrtEngine and the single thread that is allowed to touch it
/// (design doc §4.2 — the C API requires every call to come from one
/// thread). Everything else (Rhino's main thread, RealtimeDisplayMode
/// callbacks) only ever sets thread-safe flags; the render thread is the
/// only place that calls into <see cref="MrtEngine"/>.
///
/// Scene sync is injected via the syncScene callback rather than owned here,
/// so this class has no dependency on ChangeQueue/RhinoDoc (D1+): it runs on
/// the render thread, right before each commit, and is expected to call
/// mrtAdd/Update/Remove* on the engine for whatever changed (matches
/// MrtChangeQueue.Flush()'s semantics — see docs/rhino-integration-design.md §4.2).
/// </summary>
public sealed class RenderSession : IDisposable
{
    private readonly object _controlLock = new();
    private readonly ManualResetEventSlim _runGate = new(initialState: true); // set = running, reset = paused
    private readonly CancellationTokenSource _cts = new();
    private readonly Action<MrtEngine> _syncScene;
    private readonly Action<MrtEngine, RenderSessionFrame>? _onFrameRendered;
    private readonly Action<Exception>? _onFatalError;

    private readonly uint _engineWidth, _engineHeight;
    private readonly bool _enableValidation;
    private readonly uint _gpuIndex1;

    private Thread? _thread;
    private MrtEngine? _engine;
    private volatile bool _paused;

    // The control side's view of "what the engine's render settings should
    // be" - Resize() and UpdateRenderSettings() both read-modify-write this
    // under _controlLock rather than touching MrtRenderSettings.Width/Height
    // independently, so a resize during interactive down-res (E2) can never
    // clobber a user's spp/bounces/denoise/exposure choice (E5) or vice
    // versa - the earlier version of this class had exactly that bug: every
    // resize reset the whole settings struct back to MrtRenderSettings.Default.
    private MrtRenderSettings _lastRequestedSettings;
    private bool _settingsPending;

    public bool IsRunning => _thread is { IsAlive: true };
    public bool IsPaused => _paused;

    public RenderSession(uint width, uint height, Action<MrtEngine> syncScene,
        Action<MrtEngine, RenderSessionFrame>? onFrameRendered = null,
        Action<Exception>? onFatalError = null,
        bool enableValidation = false, uint gpuIndex1 = 0,
        MrtRenderSettings? initialSettings = null)
    {
        _syncScene = syncScene;
        _onFrameRendered = onFrameRendered;
        _onFatalError = onFatalError;
        _engineWidth = width;
        _engineHeight = height;
        _enableValidation = enableValidation;
        _gpuIndex1 = gpuIndex1;

        _lastRequestedSettings = initialSettings ?? MrtRenderSettings.Default;
        _lastRequestedSettings.Width = width;
        _lastRequestedSettings.Height = height;
    }

    /// <summary>Starts the render thread. Safe to call once; call Shutdown() before reusing.</summary>
    public void Start()
    {
        lock (_controlLock)
        {
            if (_thread is not null) return;
            _thread = new Thread(RenderLoop) { Name = "MiniRaytrace RenderSession", IsBackground = true };
            _thread.Start();
        }
    }

    /// <summary>
    /// Pauses rendering: the render thread blocks (no GPU work at all) until
    /// Resume() — verified in B3's stress test that this drops GPU usage to
    /// zero, not just "renders less often".
    /// </summary>
    public void Pause()
    {
        _paused = true;
        _runGate.Reset();
    }

    public void Resume()
    {
        _paused = false;
        _runGate.Set();
    }

    /// <summary>Queues a resize, applied on the render thread before the next commit.</summary>
    public void Resize(uint width, uint height)
    {
        lock (_controlLock)
        {
            _lastRequestedSettings.Width = width;
            _lastRequestedSettings.Height = height;
            _settingsPending = true;
        }
    }

    /// <summary>
    /// Queues an spp-limit/bounces/denoise/exposure change (issue E5),
    /// applied on the render thread before the next commit. Width/Height are
    /// preserved from whatever Resize() last set (or the constructor size).
    /// </summary>
    public void UpdateRenderSettings(uint sppLimit, uint maxBounces, bool denoise, float exposureEV)
    {
        lock (_controlLock)
        {
            _lastRequestedSettings.SppLimit = sppLimit;
            _lastRequestedSettings.MaxBounces = maxBounces;
            _lastRequestedSettings.Denoise = denoise ? 1 : 0;
            _lastRequestedSettings.ExposureEV = exposureEV;
            _settingsPending = true;
        }
    }

    /// <summary>
    /// Stops the render thread and destroys the engine, waiting up to
    /// <paramref name="timeoutMs"/> for a clean join. Rhino must never hang
    /// on exit because of a stuck session (design doc §4.2) — if the thread
    /// doesn't join in time this returns false but still lets the process
    /// continue; the thread is a background thread so it won't itself keep
    /// the process alive.
    /// </summary>
    public bool Shutdown(int timeoutMs = 5000)
    {
        Thread? thread;
        lock (_controlLock)
        {
            thread = _thread;
            _thread = null;
        }
        if (thread is null) return true;

        _cts.Cancel();
        _runGate.Set(); // wake up if paused, so the loop can observe cancellation
        return thread.Join(timeoutMs);
    }

    public void Dispose() => Shutdown();

    private void RenderLoop()
    {
        MrtEngine engine;
        try
        {
            engine = MrtEngine.Create(_engineWidth, _engineHeight, _enableValidation, _gpuIndex1);
            _engine = engine;
            MrtRenderSettings initial;
            lock (_controlLock) { initial = _lastRequestedSettings; _settingsPending = false; }
            engine.SetRenderSettings(initial);
        }
        catch (Exception ex)
        {
            _onFatalError?.Invoke(ex);
            return;
        }

        try
        {
            while (!_cts.IsCancellationRequested)
            {
                // Blocks with zero GPU usage while paused (B3 acceptance);
                // also wakes immediately on Shutdown() via the cancellation token.
                try { _runGate.Wait(_cts.Token); }
                catch (OperationCanceledException) { break; }
                if (_cts.IsCancellationRequested) break;

                if (_settingsPending)
                {
                    MrtRenderSettings settings;
                    lock (_controlLock) { settings = _lastRequestedSettings; _settingsPending = false; }
                    engine.SetRenderSettings(settings);
                }

                _syncScene(engine);
                engine.CommitScene();
                MrtFrameInfo info = engine.RenderFrame();
                // Runs on the render thread, so it's safe (and expected) for
                // the callback to call engine.ReadFramebuffer() here — e.g.
                // to write the frame into a Rhino RenderWindow (E1).
                _onFrameRendered?.Invoke(engine, new RenderSessionFrame(info.Spp, info.Converged != 0, info.Denoised != 0, info.FrameMs));

                // Converged and idle: renderFrame()/commitScene() are cheap
                // no-ops here, but calling them in a tight loop would still
                // spin a full CPU core for nothing. A short sleep is not the
                // same mechanism as Pause() (GPU work is already zero here;
                // this just gives CPU back) but serves the same "don't waste
                // resources when there's nothing to do" goal.
                if (info.Converged != 0) Thread.Sleep(16);
            }
        }
        catch (Exception ex)
        {
            _onFatalError?.Invoke(ex);
        }
        finally
        {
            engine.Dispose();
            _engine = null;
        }
    }
}
