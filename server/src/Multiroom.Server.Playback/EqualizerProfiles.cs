using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Playback;

// A named equalizer. Applying one copies its
// preamp and bands into a room; changing it later changes no room. BuiltIn
// ones cannot be changed or removed.
public sealed record EqualizerProfile(string Name, double PreampDb, IReadOnlyList<CoreEqBand> Bands, bool BuiltIn = false)
{
    public bool Equals(EqualizerProfile? other) =>
        other is not null && Name == other.Name && PreampDb.Equals(other.PreampDb) && BuiltIn == other.BuiltIn &&
        Bands.SequenceEqual(other.Bands);

    public override int GetHashCode() => HashCode.Combine(Name, PreampDb, Bands.Count);
}

// The house's profiles: the built-in ones, then the user's by name. Kept by
// PlaybackStore; a name is matched in any case.
public sealed class EqualizerProfiles
{
    // The ServerGui's Basic mode: ten peaks an octave apart.
    public static readonly IReadOnlyList<double> BasicFrequencies =
        [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];

    public const double BasicQ = 1.41;

    // Each preamp takes the profile's highest boost back to 0 dB (the
    // ServerGui's automatic headroom), so no built-in clips a full-scale signal.
    public static readonly IReadOnlyList<EqualizerProfile> BuiltIns =
    [
        Basic("Flat", 0, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
        Basic("Bass Boost", -7.2, [6, 5, 4, 2, 0, 0, 0, 0, 0, 0]),
        Basic("Vocal", -4, [-2, -2, -1, 0, 2, 3, 3, 2, 0, -1]),
        Basic("Night", 0, [-6, -4, -2, 0, 0, 0, 0, -1, -2, -4]),
    ];

    private readonly Lock _lock = new();
    private readonly SortedDictionary<string, EqualizerProfile> _user = new(StringComparer.OrdinalIgnoreCase);

    // Raised after a change, never while restoring.
    public event EventHandler? Changed;

    public IReadOnlyList<EqualizerProfile> All
    {
        get
        {
            lock (_lock)
                return [.. BuiltIns, .. _user.Values];
        }
    }

    public EqualizerProfile? Find(string name)
    {
        lock (_lock)
            return BuiltIns.FirstOrDefault(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase)) ??
                _user.GetValueOrDefault(name);
    }

    // Creates or replaces a user profile.
    public Result<EqualizerProfile, CoreError> Save(string name, double preampDb, IReadOnlyList<CoreEqBand> bands)
    {
        name = name.Trim();
        if (name.Length is 0 or > 64)
            return Result.Fail(CoreError.InvalidArgument, "a profile's name is 1 to 64 characters");
        if (IsBuiltIn(name))
            return Result.Fail(CoreError.Conflict, $"{name} is a built-in profile");
        if (new CoreEqualizer(true, preampDb, bands).Invalid() is { } invalid)
            return Result.Fail(CoreError.InvalidArgument, invalid);

        var profile = new EqualizerProfile(name, preampDb, [.. bands]);
        lock (_lock)
        {
            if (_user.GetValueOrDefault(name) == profile)
                return profile;
            _user.Remove(name);
            _user[name] = profile;
        }
        Changed?.Invoke(this, EventArgs.Empty);
        return profile;
    }

    public Result<bool, CoreError> Remove(string name)
    {
        if (IsBuiltIn(name))
            return Result.Fail(CoreError.Conflict, $"{name} is a built-in profile");
        lock (_lock)
        {
            if (!_user.Remove(name))
                return Result.Fail(CoreError.NotFound, $"no equalizer profile {name}");
        }
        Changed?.Invoke(this, EventArgs.Empty);
        return true;
    }

    // At startup, before anyone reads them. A saved profile that now clashes
    // with a built-in is dropped.
    public void Restore(IEnumerable<EqualizerProfile> saved)
    {
        lock (_lock)
        {
            foreach (var profile in saved.Where(p => !IsBuiltIn(p.Name)))
                _user[profile.Name] = profile with { BuiltIn = false };
        }
    }

    public IReadOnlyList<EqualizerProfile> Save()
    {
        lock (_lock)
            return [.. _user.Values];
    }

    private static bool IsBuiltIn(string name) =>
        BuiltIns.Any(p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase));

    private static EqualizerProfile Basic(string name, double preampDb, double[] gains) =>
        new(name, preampDb, [.. BasicFrequencies.Select((f, i) => new CoreEqBand(EqFilterType.Peak, f, BasicQ, gains[i]))],
            BuiltIn: true);
}
