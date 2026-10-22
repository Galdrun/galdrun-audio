using System.Collections.Concurrent;
using Multiroom.Server.Library;

namespace Multiroom.Server.Playback;

// Who is listening in each zone (phase 36): a user UserStore knows, or ""
// (the household) until a UI says otherwise; nothing guesses it. Set through
// House, kept in playback.json, and read by PlayHistoryRecorder the moment a
// track becomes audible. A user removed leaves their zones to the household.
public sealed class ZoneUsers
{
    private readonly ConcurrentDictionary<string, string> _users = new(StringComparer.Ordinal);

    public ZoneUsers(UserStore store) => store.Removed += (_, name) =>
    {
        foreach (var (zone, user) in _users)
        {
            if (string.Equals(user, name, StringComparison.OrdinalIgnoreCase))
                Set(zone, UserStore.Household);
        }
    };

    // A zone's user changed, by its zone; never raised while restoring.
    public event EventHandler<string>? Changed;

    public string Of(string zone) => _users.GetValueOrDefault(zone, UserStore.Household);

    // The user as UserStore resolved them.
    public void Set(string zone, string user)
    {
        var changed = user.Length == 0
            ? _users.TryRemove(zone, out _)
            : _users.TryGetValue(zone, out var before) ? before != user && _users.TryUpdate(zone, user, before) : _users.TryAdd(zone, user);
        if (changed)
            Changed?.Invoke(this, zone);
    }

    public IReadOnlyDictionary<string, string> Save() => new Dictionary<string, string>(_users, StringComparer.Ordinal);

    // At startup, before anyone reads them.
    public void Restore(IReadOnlyDictionary<string, string> saved)
    {
        foreach (var (zone, user) in saved.Where(u => u.Value.Length > 0))
            _users[zone] = user;
    }
}
