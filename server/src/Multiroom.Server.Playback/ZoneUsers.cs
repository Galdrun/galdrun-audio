using System.Collections.Concurrent;
using Multiroom.Server.Library;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Playback;

// Who listens in each room (phase 36, per room since): a user UserStore
// knows, or "" (the household) until a UI says otherwise; nothing guesses it.
// A zone's listener follows from its active rooms: the one listener they all
// have, else the household. Set through House, kept in playback.json, and
// read by PlayHistoryRecorder the moment a track becomes audible. A user
// removed leaves their rooms to the household.
public sealed class ZoneUsers
{
    private readonly ConcurrentDictionary<string, string> _users = new(StringComparer.Ordinal);
    private readonly CoreLink _link;

    public ZoneUsers(UserStore store, CoreLink link)
    {
        _link = link;
        store.Removed += (_, name) =>
        {
            foreach (var (room, user) in _users)
            {
                if (string.Equals(user, name, StringComparison.OrdinalIgnoreCase))
                    Set(room, UserStore.Household);
            }
        };
    }

    // A room's user changed, by its client id; never raised while restoring.
    public event EventHandler<string>? Changed;

    public string OfRoom(string client) => _users.GetValueOrDefault(client, UserStore.Household);

    // The zone's active rooms' one listener, else the household.
    public string Of(string zone)
    {
        var members = _link.Desired.MembersOf(zone) is { Count: > 0 } wanted
            ? wanted
            : _link.Model.Current.Zones.GetValueOrDefault(zone)?.Clients ?? [];
        var listeners = members.Where(c => _link.Model.Current.Rooms.GetValueOrDefault(c)?.Active != false)
            .Select(OfRoom).Distinct(StringComparer.Ordinal).Take(2).ToList();
        return listeners is [var one] ? one : UserStore.Household;
    }

    // The user as UserStore resolved them.
    public void Set(string client, string user)
    {
        var changed = user.Length == 0
            ? _users.TryRemove(client, out _)
            : _users.TryGetValue(client, out var before) ? before != user && _users.TryUpdate(client, user, before) : _users.TryAdd(client, user);
        if (changed)
            Changed?.Invoke(this, client);
    }

    public IReadOnlyDictionary<string, string> Save() => new Dictionary<string, string>(_users, StringComparer.Ordinal);

    // At startup, before anyone reads them.
    public void Restore(IReadOnlyDictionary<string, string> saved)
    {
        foreach (var (room, user) in saved.Where(u => u.Value.Length > 0))
            _users[room] = user;
    }
}
