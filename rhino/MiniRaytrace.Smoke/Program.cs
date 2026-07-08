// Console smoke test for the P/Invoke layer (design doc §8 B2) and the
// render-thread lifecycle (§8 B3): create an engine, add a triangle,
// render, read back pixels — all without Rhino installed or running. Also
// asserts native/managed struct sizes stay in sync, since that drift is
// otherwise silent and dangerous.

using MiniRaytrace.Rhino.Interop;
using MiniRaytrace.Rhino.Session;

int failures = 0;

void Check(bool condition, string what)
{
    if (condition)
    {
        Console.WriteLine($"  [pass] {what}");
    }
    else
    {
        Console.WriteLine($"  [FAIL] {what}");
        failures++;
    }
}

Console.WriteLine("== struct size sanity ==");
// These must match core/src/render/GpuTypes.hpp / capi/include/mini_raytrace.h
// exactly; drift here silently corrupts native memory instead of erroring.
Check(System.Runtime.InteropServices.Marshal.SizeOf<MrtVec3>() == 12, "MrtVec3 == 12 bytes");
Check(System.Runtime.InteropServices.Marshal.SizeOf<MrtCameraDescEx>() == 8 + 4 + 12 * 3 + 4 * 4,
    "MrtCameraDescEx size matches C layout");

Console.WriteLine("== engine lifecycle + render ==");
using (var engine = MrtEngine.Create(width: 64, height: 64, enableValidation: true))
{
    Check(true, "MrtEngine.Create succeeded");

    float[] positions =
    {
        -0.6f, -0.6f, 0.0f,
         0.6f, -0.6f, 0.0f,
         0.0f,  0.6f, 0.0f,
    };
    uint[] indices = { 0, 1, 2 };
    uint mesh = engine.AddMesh(positions, indices);
    Check(mesh != 0, "AddMesh returned a valid id");

    var material = MrtMaterialDesc.Default;
    material.BaseColor = new MrtVec3(1.0f, 0.2f, 0.1f);
    material.Emission = new MrtVec3(2.0f, 0.4f, 0.2f); // emissive so it's visible with zero lights
    uint mat = engine.AddMaterial(material);

    uint instance = engine.AddInstance(mesh, xform3x4: null, mat);
    Check(instance != 0, "AddInstance returned a valid id");

    var cam = MrtCameraDesc.Default;
    cam.Position = new MrtVec3(0, 0, 3);
    cam.Target = new MrtVec3(0, 0, 0);
    engine.SetCamera(cam);

    var settings = MrtRenderSettings.Default;
    settings.Width = 64;
    settings.Height = 64;
    settings.SppLimit = 4;
    settings.Denoise = 0;
    engine.SetRenderSettings(settings);

    var stats = engine.CommitScene();
    Check(stats.BlasRebuilt == 1, $"commitScene rebuilt exactly one BLAS (got {stats.BlasRebuilt})");

    MrtFrameInfo info = default;
    for (int i = 0; i < 4; ++i) info = engine.RenderFrame();
    Check(info.Spp == 4, $"rendered to spp=4 (got {info.Spp})");

    byte[] rgba8 = new byte[64 * 64 * 4];
    engine.ReadFramebuffer(MrtPixelFormat.Rgba8Srgb, rgba8);

    bool anyNonBlack = false;
    for (int i = 0; i < rgba8.Length; i += 4)
        if (rgba8[i] > 10 || rgba8[i + 1] > 10 || rgba8[i + 2] > 10) { anyNonBlack = true; break; }
    Check(anyNonBlack, "readback produced a non-black image");

    float[] rgba32f = new float[64 * 64 * 4];
    Span<byte> asBytes = System.Runtime.InteropServices.MemoryMarshal.AsBytes(rgba32f.AsSpan());
    engine.ReadFramebuffer(MrtPixelFormat.Rgba32fLinear, asBytes);
    bool allOpaque = true;
    for (int i = 3; i < rgba32f.Length; i += 4) if (rgba32f[i] != 1.0f) { allOpaque = false; break; }
    Check(allOpaque, "RGBA32F alpha is always 1.0");
}
Check(true, "MrtEngine disposed cleanly (mrtDestroyEngine)");

Console.WriteLine("== RenderSession lifecycle stress (B3) ==");
{
    long startMem = GC.GetTotalMemory(forceFullCollection: true);
    bool anySessionFailed = false;
    const int kIterations = 30;

    for (int i = 0; i < kIterations; ++i)
    {
        uint framesRendered = 0;
        Exception? fatal = null;
        // Empty-scene syncScene: this test is about the thread lifecycle
        // (start/pause/resume/shutdown), not rendering correctness — that's
        // already covered above and by the native test suite.
        var session = new RenderSession(
            width: 48, height: 48,
            syncScene: static _ => { },
            onFrameRendered: (_, _) => framesRendered++,
            onFatalError: ex => fatal = ex);

        session.Start();
        // Let it render a few (empty-scene, but that's fine — B3 tests the
        // thread lifecycle, not rendering correctness) frames.
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (framesRendered < 2 && fatal is null && sw.ElapsedMilliseconds < 5000) Thread.Sleep(10);

        session.Pause();
        Thread.Sleep(20);
        session.Resume();
        Thread.Sleep(20);

        bool joined = session.Shutdown(timeoutMs: 5000);
        if (!joined || fatal is not null || framesRendered == 0) anySessionFailed = true;
    }

    Check(!anySessionFailed, $"{kIterations} start/pause/resume/shutdown cycles all completed cleanly");

    long endMem = GC.GetTotalMemory(forceFullCollection: true);
    double growthMb = (endMem - startMem) / (1024.0 * 1024.0);
    Console.WriteLine($"  [diag] managed heap growth after {kIterations} sessions: {growthMb:F2} MB");
    Check(growthMb < 20.0, "managed heap growth stays bounded (no gross per-session leak)");
}

Console.WriteLine();
Console.WriteLine(failures == 0 ? "ALL CHECKS PASSED" : $"{failures} CHECK(S) FAILED");
return failures == 0 ? 0 : 1;
