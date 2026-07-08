using Rhino.Render;

namespace MiniRaytrace.Rhino.Realtime;

/// <summary>
/// Registers <see cref="MrtRealtimeDisplayMode"/> with Rhino's viewport
/// display-mode dropdown. Rhino discovers this automatically from any
/// RealtimeDisplayMode + RealtimeDisplayModeClassInfo pair in the assembly —
/// no explicit RegisterDisplayModes() call needed (confirmed against
/// McNeel's MockingBird sample, MockingRenderPlugIn.cs).
///
/// GUID is permanent once shipped: Rhino persists the user's chosen display
/// mode by this GUID (design doc §3). NEVER change it after release.
/// </summary>
public class MrtRealtimeDisplayModeInfo : RealtimeDisplayModeClassInfo
{
    public override string Name => "MiniRaytrace";

    public override Guid GUID => new("5380FD08-53AF-491D-BC5E-1A5661FF2CCC");

    public override Type RealtimeDisplayModeType => typeof(MrtRealtimeDisplayMode);

    public override bool DrawOpenGl => false;
}
