namespace MiniRaytrace.Rhino.ChangeQueue;

/// <summary>
/// Rhino-side identifier -> core handle tables (design doc §5.1), owned by
/// <see cref="MrtChangeQueue"/> for the lifetime of one session.
/// </summary>
internal sealed class HandleMaps
{
    /// <summary>What a live mrtInstanceId currently points at, so a material
    /// reassignment can remove+recreate it (there is no mrtSetInstanceMaterial
    /// in the C ABI - see MaterialSync.EnsureInstanceMaterial).</summary>
    public sealed record InstanceRecord(uint MrtInstanceId, Guid MeshId, int MeshIndex, uint MaterialCrc);

    // ChangeQueue splits one Rhino mesh into one sub-mesh per material;
    // meshIndex is that split's index - matches the core's "one material per
    // mesh" model with zero translation (design doc §5.1).
    public readonly Dictionary<(Guid MeshId, int MeshIndex), uint> Meshes = new();

    // ChangeQueue MeshInstance.InstanceId -> our record of it.
    public readonly Dictionary<uint, InstanceRecord> Instances = new();

    // Material content crc (ChangeQueue's Material.Id / MeshInstance.MaterialId) -> mrtMaterialId.
    public readonly Dictionary<uint, uint> Materials = new();

    // RenderTexture crc -> mrtTextureId (bake cache, §5.3).
    public readonly Dictionary<uint, uint> Textures = new();

    // Rhino light Guid -> mrtLightId.
    public readonly Dictionary<Guid, uint> Lights = new();

    public void Clear()
    {
        Meshes.Clear();
        Instances.Clear();
        Materials.Clear();
        Textures.Clear();
        Lights.Clear();
    }
}
