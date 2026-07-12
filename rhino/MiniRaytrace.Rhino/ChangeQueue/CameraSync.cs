using MiniRaytrace.Rhino.Interop;
using Rhino.DocObjects;
using Rhino.Geometry;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Camera translation (design doc §5.4, issue D7): Rhino's possibly-
/// asymmetric frustum (pan creates an off-center frustum) and three
/// projection modes (perspective / parallel / two-point) map onto
/// <see cref="MrtCameraDescEx"/> almost field-for-field via
/// <see cref="ViewportInfo.GetFrustum"/> - two-point perspective needs no
/// special case, an asymmetric top/bottom with a level forward already
/// expresses it.
/// </summary>
public sealed partial class MrtChangeQueue
{
    protected override void ApplyViewChange(ViewInfo viewInfo) =>
        Guarded(nameof(ApplyViewChange), () => ApplyViewChangeCore(viewInfo));

    private void ApplyViewChangeCore(ViewInfo viewInfo)
    {
        var engine = Engine ?? throw new InvalidOperationException("MrtChangeQueue.Engine not set");
        ViewportInfo vp = viewInfo.Viewport;
        if (!vp.IsValidCamera || !vp.IsValidFrustum) return;

        if (!string.IsNullOrEmpty(viewInfo.WallpaperFilename))
            ChangeQueueLog.WarnOnce("wallpaper", "wallpaper backgrounds aren't supported, ignored");

        vp.GetFrustum(out double left, out double right, out double bottom, out double top, out double near, out _);

        var desc = MrtCameraDescEx.Default;
        desc.Projection = vp.IsPerspectiveProjection ? MrtCameraProjection.Perspective : MrtCameraProjection.Orthographic;
        desc.Position = ToVec3(vp.CameraLocation);

        Vector3d forward = vp.CameraDirection;
        forward.Unitize();
        Vector3d up = vp.CameraUp;
        up.Unitize();
        desc.Forward = ToVec3(forward);
        desc.Up = ToVec3(up);

        // GetFrustum's l/r/b/t are measured at the near plane; mrtCameraDescEx
        // wants perspective slices normalized to dist=1 (design doc §5.4).
        // Orthographic ones are absolute sizes already, no near division.
        double scale = desc.Projection == MrtCameraProjection.Perspective && near > 1e-9 ? 1.0 / near : 1.0;
        desc.Left = (float)(left * scale);
        desc.Right = (float)(right * scale);
        desc.Bottom = (float)(bottom * scale);
        desc.Top = (float)(top * scale);

        engine.SetCameraEx(desc);
        NoteCameraForward(desc.Forward);
        RaiseViewChanged();
    }
}
