using Rhino;

namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Rhino command-line logging for the translation layer. Several design doc
/// §5.3 fallbacks ("discard clearcoat", "WCS mapping -> constant color",
/// "spot light downgraded") are meant to log once per offending
/// object/material, not spam every Flush - <see cref="WarnOnce"/> is that.
/// </summary>
internal static class ChangeQueueLog
{
    private static readonly HashSet<string> SeenOnce = new();

    /// <summary>Toggled by the diagnostic mode mentioned in D1's acceptance criteria.</summary>
    public static bool DiagnosticsEnabled = false;

    // Logging runs inside ChangeQueue callbacks invoked from native RDK
    // code, where an escaping exception kills the Rhino process - so the
    // logger itself must be incapable of throwing.
    public static void Warn(string message)
    {
        try { RhinoApp.WriteLine($"MiniRaytrace: {message}"); }
        catch { }
    }

    public static void WarnOnce(string key, string message)
    {
        if (SeenOnce.Add(key)) Warn(message);
    }

    public static void Diag(string message)
    {
        if (!DiagnosticsEnabled) return;
        try { RhinoApp.WriteLine($"MiniRaytrace[diag]: {message}"); }
        catch { }
    }
}
