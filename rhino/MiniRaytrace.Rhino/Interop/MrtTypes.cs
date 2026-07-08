using System.Runtime.InteropServices;

namespace MiniRaytrace.Rhino.Interop;

// Blittable mirrors of mini_raytrace.h. Field order/size must stay byte-
// identical with the C structs — there's a size assertion test
// (NativeStructSizeTests, see MiniRaytrace.Smoke) that catches drift.

[StructLayout(LayoutKind.Sequential)]
public struct MrtVec3
{
    public float X, Y, Z;
    public MrtVec3(float x, float y, float z) { X = x; Y = y; Z = z; }
}

public enum MrtResult : int
{
    Success = 0,
    ErrorUnknown,
    ErrorVulkanInit,
    ErrorOutOfMemory,
    ErrorInvalidArgument,
    ErrorInvalidHandle,
    ErrorShaderLoad,
    ErrorDeviceLost,
}

public enum MrtPixelFormat : int
{
    Rgba8Srgb = 0,
    Rgba32fLinear = 1,
}

public enum MrtTextureFormat : int
{
    Rgba8Srgb = 0,
    Rgba8Unorm = 1,
    Rgba32f = 2,
}

public enum MrtLightType : int
{
    Sun = 0,
    Point = 1,
    Rect = 2,
}

public enum MrtTonemap : int
{
    Linear = 0,
    Aces = 1,
}

public enum MrtCameraProjection : int
{
    Perspective = 0,
    Orthographic = 1,
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtEngineDesc
{
    public nuint StructSize;
    public int EnableValidation;
    public uint Width, Height;
    public uint GpuIndex1; // 0 = auto; N = force device index N-1 (PRD §8 A4)

    public static MrtEngineDesc Default => new()
    {
        StructSize = (nuint)Marshal.SizeOf<MrtEngineDesc>(),
    };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtMeshDesc
{
    public nuint StructSize;
    public uint VertexCount;
    public uint IndexCount;
    public IntPtr Positions; // const float*, xyz per vertex, required
    public IntPtr Normals;   // const float*, xyz, optional
    public IntPtr Uvs;       // const float*, uv, optional
    public IntPtr Indices;   // const uint32_t*, required

    public static MrtMeshDesc Default => new() { StructSize = (nuint)Marshal.SizeOf<MrtMeshDesc>() };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtMaterialDesc
{
    public nuint StructSize;
    public MrtVec3 BaseColor;
    public float Opacity;
    public float Roughness;
    public float Metallic;
    public float Transmission;
    public float Ior;
    public MrtVec3 Emission;
    public uint BaseColorTex, RoughnessTex, MetallicTex, NormalTex, EmissionTex;

    public static MrtMaterialDesc Default => new()
    {
        StructSize = (nuint)Marshal.SizeOf<MrtMaterialDesc>(),
        BaseColor = new MrtVec3(0.8f, 0.8f, 0.8f),
        Opacity = 1.0f,
        Roughness = 0.5f,
        Ior = 1.45f,
    };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtTextureDesc
{
    public nuint StructSize;
    public uint Width, Height;
    public MrtTextureFormat Format;
    public IntPtr Pixels; // const void*, tightly packed RGBA
    public int GenerateMips;

    public static MrtTextureDesc Default => new() { StructSize = (nuint)Marshal.SizeOf<MrtTextureDesc>() };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtLightDesc
{
    public nuint StructSize;
    public MrtLightType Type;
    public MrtVec3 Direction;
    public MrtVec3 Position;
    public MrtVec3 Radiance;
    public float AngularRadius;
    public MrtVec3 Corner, Edge0, Edge1;
    public int TwoSided;

    public static MrtLightDesc Default => new()
    {
        StructSize = (nuint)Marshal.SizeOf<MrtLightDesc>(),
        Direction = new MrtVec3(0, 1, 0),
        Radiance = new MrtVec3(1, 1, 1),
        AngularRadius = 0.00465f,
        Edge0 = new MrtVec3(1, 0, 0),
        Edge1 = new MrtVec3(0, 0, 1),
    };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtCameraDesc
{
    public nuint StructSize;
    public MrtVec3 Position;
    public MrtVec3 Target;
    public MrtVec3 Up;
    public float FovYDeg;

    public static MrtCameraDesc Default => new()
    {
        StructSize = (nuint)Marshal.SizeOf<MrtCameraDesc>(),
        Position = new MrtVec3(0, 0, 5),
        Up = new MrtVec3(0, 1, 0),
        FovYDeg = 45.0f,
    };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtCameraDescEx
{
    public nuint StructSize;
    public MrtCameraProjection Projection;
    public MrtVec3 Position;
    public MrtVec3 Forward;
    public MrtVec3 Up;
    public float Left, Right, Bottom, Top;

    public static MrtCameraDescEx Default => new()
    {
        StructSize = (nuint)Marshal.SizeOf<MrtCameraDescEx>(),
        Forward = new MrtVec3(0, 0, -1),
        Up = new MrtVec3(0, 1, 0),
        Left = -1, Right = 1, Bottom = -1, Top = 1,
    };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtRenderSettings
{
    public nuint StructSize;
    public uint Width, Height;
    public uint MaxBounces;
    public uint SppLimit;
    public float ExposureEV;
    public float FireflyClamp;
    public MrtTonemap Tonemap;
    public int Denoise;

    public static MrtRenderSettings Default => new()
    {
        StructSize = (nuint)Marshal.SizeOf<MrtRenderSettings>(),
        MaxBounces = 8,
        SppLimit = 1024,
        FireflyClamp = 50.0f,
        Tonemap = MrtTonemap.Aces,
        Denoise = 1,
    };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtFrameInfo
{
    public nuint StructSize;
    public uint Spp;
    public int Converged;
    public int Denoised;
    public float FrameMs;

    public static MrtFrameInfo Default => new() { StructSize = (nuint)Marshal.SizeOf<MrtFrameInfo>() };
}

[StructLayout(LayoutKind.Sequential)]
public struct MrtCommitStats
{
    public nuint StructSize;
    public uint BlasRebuilt;
    public int TlasRebuilt;
    public float CommitMs;

    public static MrtCommitStats Default => new() { StructSize = (nuint)Marshal.SizeOf<MrtCommitStats>() };
}

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void MrtLogFn(int level, [MarshalAs(UnmanagedType.LPUTF8Str)] string message, IntPtr user);
