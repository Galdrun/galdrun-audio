# Listeners

A **listener** is a person in your household, such as "Anna" or "Rafael".
A listener is only a name. There is no login or password. Each listener
gets their own likes, favourites, recently played tracks, AI proposals and
audiobook positions. Anything not tied to a person belongs to the
**household**, the built-in listener everyone shares.

Without any listeners, Galdrun works exactly as before: everything is the
household's.

## Adding and removing listeners

Open **Listeners** in the server GUI's menu.

- **Add a listener:** type a name (1 to 40 characters) and choose **Add**.
  Names are unique, regardless of upper or lower case.
- **Remove a listener:** choose **✕** next to their name. Galdrun asks what
  should happen to their likes, favourites, played tracks and audiobook
  positions:
  - **Move to household:** they become the household's. If the household
    already has the same item, it is kept only once.
  - **Delete them:** they are deleted together with the listener.

  Their AI proposals are deleted either way.

## Who listens in which room

Each **room** has a listener, and the household is the default. You can set
it in three places:

- the **Who listens where** table on the **Listeners** page,
- the **Listening** section of a room's settings page,
- the listener button on a zone's card. This sets the listener of every room
  in that zone.

A **zone's** listener depends on its rooms:

| The zone's rooms | The zone's listener |
| --- | --- |
| One room | That room's listener |
| Several rooms, all with the same listener | That listener |
| Several rooms with different listeners | The household |

When rooms are grouped or ungrouped, the zone's listener updates
automatically. Each room keeps its own setting, so Anna's room is Anna's
again as soon as it plays on its own.

## What the zone's listener changes

Whatever plays in a zone counts for the zone's listener:

- **Last played:** each track is recorded for the zone's listener at the
  moment it starts playing.
- **Audiobooks:** each listener has their own position in every book. When a
  book is queued, it resumes from the listener's position. If they have none,
  it resumes from the household's position.
- **AutoFill:** AutoFill still works from the zone's current queue. The
  liked tracks in that queue, which AutoFill prefers as a starting point, are
  now the ones the zone's listener (or the household) liked.
- **AI chat:** a request made for a zone is treated as coming from that
  zone's listener, unless the request names someone else.

## The listener at the top of the GUI

The 👤 button in the server GUI's header chooses whose things you see and
make *in this browser*:

- **Home** shows that listener's likes, favourites, recently played tracks
  and proposals, together with the household's.
- A like or favourite you add in the GUI belongs to that listener.
- To see everybody's items at once, open **All** in the library (below).

The browser remembers your choice and restores it when you reload the page.
Other browsers and the apps keep their own choice.

## The library

**Favourites**, **Liked**, **Played lately** and **Proposals** in the
library work as follows:

- Opening the section itself follows the header's listener, as Home does.
- Once at least one listener exists, each section also lists:
  - **All:** everyone's items. Each one is marked with 👤 and the name of
    whoever it belongs to. Under Proposals, every listener's proposals are
    listed under their name.
  - **Household:** only the household's items.
  - **One entry per listener:** only that listener's own items, without the
    household's.

## Proposals

The AI makes proposals separately for the household and for each listener
who has likes or favourites of their own. Each set is based on that
listener's taste. **Fresh picks** in a listener's Proposals entry asks the
AI again for that listener only. Dismissing a proposal (✕) hides it only for
that listener.

## For integrators

- `PATCH /api/v1/rooms/{client}` with `{"user": "Anna"}` sets a room's
  listener; `""` sets it back to the household. Room views include `user`,
  and so do zone views, where the value is the derived zone listener. The
  events are `room.userChanged` and `zone.userChanged`.
- Reads take `?user=<name>`. That listener's own items are combined with the
  household's, and `user=*` returns everyone's. Add `&only=true` to get the
  listener's own items alone. This works for `likes`, `favorites` and
  `history`. `books/positions` takes `?user=` too.
- `DELETE /api/v1/users/{name}?data=household|delete` removes a listener. The
  default is `household`.
- The zone-level `PATCH /api/v1/zones/{zone} {"user": …}` no longer exists.
  An older `playback.json` with zone listeners is converted on first start:
  each zone's listener is given to the rooms that zone named.
