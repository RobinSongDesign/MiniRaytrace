using MiniRaytrace.Rhino.Interop;
using Rhino.DocObjects;
using Rhino.Geometry;
using RCQ = Rhino.Render.ChangeQueue;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Translates one Rhino document/view into mrt* calls (design doc §5, issue
/// D1). One instance per active session (one realtime viewport, or one modal
/// _Render) - same lifetime as its RenderSession/MrtEngine.
///
/// All the actual translation lives in the sibling partial-class files in
/// this folder (MeshSync/MaterialSync/TextureBaker/LightSync/
/// EnvironmentSync/CameraSync/GroundPlaneSync), split by design doc §5.2 row
/// for readability. This file only has construction, the handle maps, and
/// the update-batching flag.
///
/// Threading: <c>CreateWorld()</c> and <c>Flush()</c> BOTH run every
/// ApplyXxxChanges override below SYNCHRONOUSLY on the calling thread -
/// CreateWorld() is not a deferred "mark for rebuild"; it delivers
/// ApplyViewChange (and the rest of the world) before it even returns.
/// Verified the hard way: calling it from StartRenderer on the UI thread,
/// before Engine was set, crashed the whole Rhino process in the first live
/// test. Both must only ever be called from the render thread, after Engine
/// is assigned (RenderSession's syncScene callback - see
/// Session/RenderSession.cs). That is what lets every mrt* call anywhere in
/// this folder happen on the render thread with zero marshaling - it's
/// also why <see cref="Engine"/> must be (re)assigned before every Flush()
/// rather than captured once in the constructor: the ChangeQueue instance
/// outlives any single engine only in theory (v1: one ChangeQueue per
/// session, same lifetime as its engine), but keeping the two decoupled
/// avoids a subtle re-entrancy trap if that ever changes.
///
/// Not unit-testable against mocked event sequences as D1's acceptance
/// criteria originally hoped: <c>Rhino.Render.ChangeQueue.Mesh</c>,
/// <c>MeshInstance</c>, <c>Material</c>, etc. all wrap native RDK pointers
/// via internal constructors - they cannot be constructed from managed test
/// code without a live Rhino process. This can only be verified inside a
/// running Rhino session (see docs/rhino-integration-design.md risk RH1).
/// </summary>
public sealed partial class MrtChangeQueue : RCQ.ChangeQueue
{
    internal readonly HandleMaps Handles = new();

    /// <summary>The only engine these callbacks may touch; set by RenderSession right before each Flush().</summary>
    public MrtEngine? Engine { get; set; }

    /// <summary>True between NotifyBeginUpdates/EndUpdates (D1 acceptance criteria).</summary>
    public bool ChangesPending { get; private set; }

    /// <summary>
    /// Fired from inside Flush() (render thread) whenever ApplyViewChange
    /// runs - i.e. the camera moved. Consumed by the realtime display mode
    /// for interactive down-res (issue E2); unused by the modal render
    /// pipeline, which never re-Flushes after its initial CreateWorld.
    /// </summary>
    public event Action? ViewChanged;

    internal void RaiseViewChanged() => ViewChanged?.Invoke();

    // ---- fallback headlight (D5/D6 gap found in the first render test) ----
    // Rhino's own renderers synthesize a "default light" when the document
    // has no lights; without one, a lights-off + skylight-off document is
    // pure black. Mirror that: a broad, soft camera headlight that exists
    // only while there are no real lights AND the environment contributes no
    // light, following the camera as it moves.
    private uint _fallbackLightId;
    private MrtVec3 _lastCameraForward = new(0, 0, -1);
    private const float FallbackConeRadius = 0.25f; // ~14°: soft, no razor shadows

    /// <summary>
    /// Runs on the render thread right after every CreateWorld()/Flush()
    /// (called by the display mode / render pipeline, not by native code):
    /// backfills an environment if no skylight/environment event ever
    /// arrived, then reconciles the fallback headlight.
    /// </summary>
    public void PostFlushFixups() =>
        Guarded(nameof(PostFlushFixups), () =>
        {
            if (Engine is null) return;
            EnsureEnvironmentInitialized();
            UpdateFallbackLight();
        });

    internal void NoteCameraForward(MrtVec3 forward)
    {
        _lastCameraForward = forward;
        // Keep the headlight glued to the view while it exists.
        if (_fallbackLightId != 0 && Engine is { } engine)
            engine.UpdateLight(_fallbackLightId, BuildFallbackLightDesc());
    }

    private void UpdateFallbackLight()
    {
        var engine = Engine!;
        bool needed = Handles.Lights.Count == 0 && !EnvironmentLit;
        if (needed && _fallbackLightId == 0)
        {
            _fallbackLightId = engine.AddLight(BuildFallbackLightDesc());
            ChangeQueueLog.Diag("fallback headlight on (no lights, environment unlit)");
        }
        else if (!needed && _fallbackLightId != 0)
        {
            engine.RemoveLight(_fallbackLightId);
            _fallbackLightId = 0;
            ChangeQueueLog.Diag("fallback headlight off");
        }
    }

    private MrtLightDesc BuildFallbackLightDesc()
    {
        var desc = MrtLightDesc.Default;
        desc.Type = MrtLightType.Sun;
        // Headlight: travels along the view direction, so "towards the
        // light" is the negated camera forward.
        desc.Direction = new MrtVec3(-_lastCameraForward.X, -_lastCameraForward.Y, -_lastCameraForward.Z);
        desc.AngularRadius = FallbackConeRadius;
        desc.Radiance = SunConeRadiance(System.Drawing.Color.White, 1.0, FallbackConeRadius);
        return desc;
    }

    /// <summary>
    /// Every Apply* override must route its body through this: these
    /// callbacks are invoked FROM NATIVE RDK CODE, and an exception escaping
    /// one unwinds through those native frames and terminates the whole
    /// Rhino process (0xe0434352 APPCRASH - observed live, not theoretical).
    /// A translation bug on one odd document must degrade to a skipped
    /// change + command-line warning, never a host crash.
    /// </summary>
    private void Guarded(string callback, Action body)
    {
        try
        {
            body();
        }
        catch (Exception ex)
        {
            ChangeQueueLog.WarnOnce($"cq-fail-{callback}-{ex.Message}",
                $"{callback} failed and this change was skipped: {ex.Message}");
        }
    }

    // GroundPlane's synthetic mesh/instance (D8) - not identified by any
    // Rhino object Guid, so it lives outside Handles.
    private uint _groundPlaneMeshId, _groundPlaneInstanceId, _groundPlaneMaterialCrc;

    public MrtChangeQueue(Guid pluginId, uint docRuntimeSerialNumber, ViewInfo viewInfo)
        : base(pluginId, docRuntimeSerialNumber, viewInfo, bRespectDisplayPipelineAttributes: true)
    {
    }

    protected override void NotifyBeginUpdates()
    {
        ChangesPending = true;
        ChangeQueueLog.Diag("NotifyBeginUpdates");
    }

    protected override void NotifyEndUpdates()
    {
        ChangesPending = false;
        ChangeQueueLog.Diag("NotifyEndUpdates");
    }

    protected override void ApplyRenderSettingsChanges(global::Rhino.Render.RenderSettings rs)
    {
        // Resolution is owned by the session (viewport size, or the document
        // render size read once in MrtRenderPlugIn.Render()), not by this
        // event - see RenderSession.Resize / MrtRenderPipeline. Only
        // genuinely document-persisted render knobs (spp limit, bounces,
        // exposure) would flow through here, and none are wired yet -
        // that's issue E5's settings panel, which will feed MrtEngine
        // through RenderSession rather than through the ChangeQueue.
        ChangeQueueLog.Diag("ApplyRenderSettingsChanges (not wired - see E5)");
    }

    private static MrtVec3 ToVec3(Point3d p) => new((float)p.X, (float)p.Y, (float)p.Z);
    private static MrtVec3 ToVec3(Vector3d v) => new((float)v.X, (float)v.Y, (float)v.Z);
}
