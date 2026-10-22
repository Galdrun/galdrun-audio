-- What the household liked: a track, an album, an artist or a station, each
-- at most once, as a reference plus a snapshot (the domain record as JSON
-- when it was liked), never a foreign key into track. The taste signal that
-- recommendations can later be drawn from.
CREATE TABLE liked (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('track', 'album', 'artist', 'station')),
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    snapshot TEXT NOT NULL,
    added INTEGER NOT NULL, -- UTC ticks
    UNIQUE (kind, provider_id, provider_item_id)
);
