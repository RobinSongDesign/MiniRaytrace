using System.Runtime.InteropServices;

namespace MiniRaytrace.Rhino.Interop;

/// <summary>
/// SafeHandle wrapper around mrtEngine* plus thin, still-1:1 convenience
/// methods (struct marshaling / array pinning done here so callers never
/// touch IntPtr). Deliberately NOT the render loop or scene-translation
/// logic — those live in Session/RenderSession and ChangeQueue/ (B3, D1+).
/// </summary>
public sealed class MrtEngineHandle : SafeHandle
{
    public MrtEngineHandle() : base(IntPtr.Zero, ownsHandle: true) { }

    public override bool IsInvalid => handle == IntPtr.Zero;

    // SafeHandle.SetHandle is protected; expose it for MrtEngine.Create,
    // which only learns the real pointer after mrtCreateEngine succeeds.
    internal void AssignHandle(IntPtr raw) => SetHandle(raw);

    protected override bool ReleaseHandle()
    {
        NativeMethods.mrtDestroyEngine(handle);
        return true;
    }
}

public sealed class MrtException(MrtResult result, string detail)
    : Exception($"{result}: {detail}")
{
    public MrtResult Result { get; } = result;
}

public sealed class MrtEngine : IDisposable
{
    private readonly MrtEngineHandle _handle;

    private MrtEngine(MrtEngineHandle handle) => _handle = handle;

    public static MrtEngine Create(uint width, uint height, bool enableValidation = false, uint gpuIndex1 = 0)
    {
        var desc = MrtEngineDesc.Default;
        desc.Width = width;
        desc.Height = height;
        desc.EnableValidation = enableValidation ? 1 : 0;
        desc.GpuIndex1 = gpuIndex1;

        MrtResult r = NativeMethods.mrtCreateEngine(ref desc, out IntPtr raw);
        if (r != MrtResult.Success)
            throw new MrtException(r, NativeMethods.mrtGetLastErrorMessage());

        var handle = new MrtEngineHandle();
        handle.AssignHandle(raw);
        return new MrtEngine(handle);
    }

    public void Dispose() => _handle.Dispose();

    public uint AddMesh(ReadOnlySpan<float> positions, ReadOnlySpan<uint> indices,
        ReadOnlySpan<float> normals = default, ReadOnlySpan<float> uvs = default)
    {
        unsafe
        {
            fixed (float* pPos = positions)
            fixed (uint* pIdx = indices)
            fixed (float* pNrm = normals)
            fixed (float* pUv = uvs)
            {
                var desc = MrtMeshDesc.Default;
                desc.VertexCount = (uint)(positions.Length / 3);
                desc.IndexCount = (uint)indices.Length;
                desc.Positions = (IntPtr)pPos;
                desc.Indices = (IntPtr)pIdx;
                desc.Normals = normals.IsEmpty ? IntPtr.Zero : (IntPtr)pNrm;
                desc.Uvs = uvs.IsEmpty ? IntPtr.Zero : (IntPtr)pUv;
                return NativeMethods.mrtAddMesh(_handle.DangerousGetHandle(), ref desc);
            }
        }
    }

    public void UpdateMesh(uint meshId, ReadOnlySpan<float> positions, ReadOnlySpan<uint> indices,
        ReadOnlySpan<float> normals = default, ReadOnlySpan<float> uvs = default)
    {
        unsafe
        {
            fixed (float* pPos = positions)
            fixed (uint* pIdx = indices)
            fixed (float* pNrm = normals)
            fixed (float* pUv = uvs)
            {
                var desc = MrtMeshDesc.Default;
                desc.VertexCount = (uint)(positions.Length / 3);
                desc.IndexCount = (uint)indices.Length;
                desc.Positions = (IntPtr)pPos;
                desc.Indices = (IntPtr)pIdx;
                desc.Normals = normals.IsEmpty ? IntPtr.Zero : (IntPtr)pNrm;
                desc.Uvs = uvs.IsEmpty ? IntPtr.Zero : (IntPtr)pUv;
                NativeMethods.mrtUpdateMesh(_handle.DangerousGetHandle(), meshId, ref desc);
            }
        }
    }

    public void RemoveMesh(uint meshId) => NativeMethods.mrtRemoveMesh(_handle.DangerousGetHandle(), meshId);

    public uint AddInstance(uint meshId, float[]? xform3x4, uint materialId) =>
        NativeMethods.mrtAddInstance(_handle.DangerousGetHandle(), meshId, xform3x4, materialId);

    public void SetInstanceTransform(uint instanceId, float[] xform3x4) =>
        NativeMethods.mrtSetInstanceTransform(_handle.DangerousGetHandle(), instanceId, xform3x4);

    public void RemoveInstance(uint instanceId) => NativeMethods.mrtRemoveInstance(_handle.DangerousGetHandle(), instanceId);

    public uint AddMaterial(MrtMaterialDesc desc) => NativeMethods.mrtAddMaterial(_handle.DangerousGetHandle(), ref desc);

    public void UpdateMaterial(uint materialId, MrtMaterialDesc desc) =>
        NativeMethods.mrtUpdateMaterial(_handle.DangerousGetHandle(), materialId, ref desc);

    public void RemoveMaterial(uint materialId) => NativeMethods.mrtRemoveMaterial(_handle.DangerousGetHandle(), materialId);

    public uint AddTexture(MrtTextureDesc desc) => NativeMethods.mrtAddTexture(_handle.DangerousGetHandle(), ref desc);

    public void RemoveTexture(uint textureId) => NativeMethods.mrtRemoveTexture(_handle.DangerousGetHandle(), textureId);

    public uint AddLight(MrtLightDesc desc) => NativeMethods.mrtAddLight(_handle.DangerousGetHandle(), ref desc);

    public void UpdateLight(uint lightId, MrtLightDesc desc) =>
        NativeMethods.mrtUpdateLight(_handle.DangerousGetHandle(), lightId, ref desc);

    public void RemoveLight(uint lightId) => NativeMethods.mrtRemoveLight(_handle.DangerousGetHandle(), lightId);

    public void ClearEnvironment() =>
        NativeMethods.mrtClearEnvironment(_handle.DangerousGetHandle(), IntPtr.Zero, 0, 1);

    public void SetEnvironment(MrtTextureDesc hdri, float rotationRad, float intensity) =>
        NativeMethods.mrtSetEnvironment(_handle.DangerousGetHandle(), ref hdri, rotationRad, intensity);

    public void SetCamera(MrtCameraDesc desc) => NativeMethods.mrtSetCamera(_handle.DangerousGetHandle(), ref desc);

    public void SetCameraEx(MrtCameraDescEx desc) => NativeMethods.mrtSetCameraEx(_handle.DangerousGetHandle(), ref desc);

    public MrtCommitStats CommitScene()
    {
        var stats = MrtCommitStats.Default;
        MrtResult r = NativeMethods.mrtCommitScene(_handle.DangerousGetHandle(), ref stats);
        if (r != MrtResult.Success) throw new MrtException(r, NativeMethods.mrtGetLastErrorMessage());
        return stats;
    }

    public void SetRenderSettings(MrtRenderSettings settings)
    {
        MrtResult r = NativeMethods.mrtSetRenderSettings(_handle.DangerousGetHandle(), ref settings);
        if (r != MrtResult.Success) throw new MrtException(r, NativeMethods.mrtGetLastErrorMessage());
    }

    public MrtFrameInfo RenderFrame()
    {
        var info = MrtFrameInfo.Default;
        MrtResult r = NativeMethods.mrtRenderFrame(_handle.DangerousGetHandle(), ref info);
        if (r != MrtResult.Success) throw new MrtException(r, NativeMethods.mrtGetLastErrorMessage());
        return info;
    }

    public void ReadFramebuffer(MrtPixelFormat format, Span<byte> dst)
    {
        unsafe
        {
            fixed (byte* p = dst)
            {
                MrtResult r = NativeMethods.mrtReadFramebuffer(_handle.DangerousGetHandle(), format, (IntPtr)p, (nuint)dst.Length);
                if (r != MrtResult.Success) throw new MrtException(r, NativeMethods.mrtGetLastErrorMessage());
            }
        }
    }

    public void ResetAccumulation() => NativeMethods.mrtResetAccumulation(_handle.DangerousGetHandle());
}
