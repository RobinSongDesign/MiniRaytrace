using System.Reflection;
using System.Runtime.InteropServices;
using Rhino.PlugIns;

// Plug-in description attributes (all optional; shown in Rhino's Options >
// Plug-ins tab).
[assembly: PlugInDescription(DescriptionType.Organization, "Robin + Claude")]
[assembly: PlugInDescription(DescriptionType.Email, "")]
[assembly: PlugInDescription(DescriptionType.WebSite, "")]

[assembly: AssemblyTitle("MiniRaytrace")] // plug-in name shown in Rhino is extracted from this
[assembly: AssemblyDescription("Vulkan compute path tracer — Rhino renderer + realtime display mode")]
[assembly: AssemblyCompany("Robin + Claude")]
[assembly: AssemblyProduct("MiniRaytrace")]
[assembly: AssemblyCopyright("Copyright 2026")]

[assembly: ComVisible(false)]

// The plug-in's identity: Rhino reads this via reflection at load time
// (PlugIn.ExtractPlugInAttributes). NEVER change once this plug-in has
// shipped — Rhino's plug-in manager and per-plugin settings are keyed by it.
[assembly: Guid("068E4F9B-040E-4524-8ADF-218FCAA857A2")]

// Keep the first three components in sync with rhino/yak/manifest.yml's
// `version:` field — `yak build` warns (not errors) on a mismatch, verified
// against a real local Yak.exe (design doc F1).
[assembly: AssemblyVersion("0.2.4.0")]
[assembly: AssemblyFileVersion("0.2.4.0")]
