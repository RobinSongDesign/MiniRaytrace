using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace MiniRaytrace.Rhino.Interop;

/// <summary>
/// P/Invoke surface for mini_raytrace.h. Classic DllImport (not the newer
/// LibraryImport source generator) — chosen deliberately so the log callback
/// delegate marshals with well-understood, long-stable semantics rather than
/// LibraryImport's newer (and pickier) function-pointer rules.
/// </summary>
internal static partial class NativeMethods
{
    private const string Lib = "mrt";

    [ModuleInitializer]
    internal static void RegisterResolver()
    {
        NativeLibrary.SetDllImportResolver(typeof(NativeMethods).Assembly, ResolveMrt);
    }

    // Loads mrt.dll / libmrt.dylib from runtimes/<rid>/native/ next to this
    // assembly (yak package layout, design doc §6.4) rather than relying on
    // PATH or the working directory — Rhino's own working directory is not
    // something a plug-in controls.
    private static IntPtr ResolveMrt(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName != Lib) return IntPtr.Zero;

        var (rid, fileName) = RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
            ? ("win-x64", "mrt.dll")
            : RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
                ? ("osx-arm64", "libmrt.dylib")
                : throw new PlatformNotSupportedException("MiniRaytrace supports Windows x64 and macOS arm64 only.");

        string? baseDir = Path.GetDirectoryName(assembly.Location);
        if (baseDir is not null)
        {
            string packaged = Path.Combine(baseDir, "runtimes", rid, "native", fileName);
            if (NativeLibrary.TryLoad(packaged, out var handle)) return handle;

            // Dev-time convenience: a locally built native lib copied next to
            // the .rhp, before a yak package layout exists.
            string sideBySide = Path.Combine(baseDir, fileName);
            if (NativeLibrary.TryLoad(sideBySide, out handle)) return handle;
        }

        throw new DllNotFoundException(
            $"Could not locate {fileName} under runtimes/{rid}/native next to {assembly.Location}. " +
            "Expected yak package layout (design doc §6.4).");
    }

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern MrtResult mrtCreateEngine(ref MrtEngineDesc desc, out IntPtr outEngine);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtDestroyEngine(IntPtr engine);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint mrtAddMesh(IntPtr engine, ref MrtMeshDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtUpdateMesh(IntPtr engine, uint meshId, ref MrtMeshDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtRemoveMesh(IntPtr engine, uint meshId);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint mrtAddInstance(IntPtr engine, uint meshId, float[]? xform3x4, uint materialId);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtSetInstanceTransform(IntPtr engine, uint instanceId, float[] xform3x4);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtRemoveInstance(IntPtr engine, uint instanceId);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint mrtAddMaterial(IntPtr engine, ref MrtMaterialDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtUpdateMaterial(IntPtr engine, uint materialId, ref MrtMaterialDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtRemoveMaterial(IntPtr engine, uint materialId);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint mrtAddTexture(IntPtr engine, ref MrtTextureDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtRemoveTexture(IntPtr engine, uint textureId);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint mrtAddLight(IntPtr engine, ref MrtLightDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtUpdateLight(IntPtr engine, uint lightId, ref MrtLightDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtRemoveLight(IntPtr engine, uint lightId);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtSetEnvironment(IntPtr engine, ref MrtTextureDesc hdri, float rotationRad, float intensity);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl, EntryPoint = "mrtSetEnvironment")]
    internal static extern void mrtClearEnvironment(IntPtr engine, IntPtr hdriNull, float rotationRad, float intensity);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtSetCamera(IntPtr engine, ref MrtCameraDesc desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtSetCameraEx(IntPtr engine, ref MrtCameraDescEx desc);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern MrtResult mrtCommitScene(IntPtr engine, ref MrtCommitStats outStats);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl, EntryPoint = "mrtCommitScene")]
    internal static extern MrtResult mrtCommitSceneNoStats(IntPtr engine, IntPtr outStatsNull);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern MrtResult mrtSetRenderSettings(IntPtr engine, ref MrtRenderSettings settings);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern MrtResult mrtRenderFrame(IntPtr engine, ref MrtFrameInfo outInfo);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern MrtResult mrtReadFramebuffer(IntPtr engine, MrtPixelFormat format, IntPtr dst, nuint dstSize);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtResetAccumulation(IntPtr engine);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void mrtSetLogCallback(MrtLogFn? fn, IntPtr user);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.LPUTF8Str)]
    internal static extern string mrtResultToString(MrtResult r);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.LPUTF8Str)]
    internal static extern string mrtGetLastErrorMessage();
}
