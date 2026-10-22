-- Favourites and playlists (phase 7): what the household chose, beside what
-- the indexer found. An entry is a reference to a provider-scoped track or
-- station plus a snapshot of it when it was added, never a foreign key into
-- track: it must outlive the provider losing the item, so the UI can say
-- what vanished. For local, provider_item_id is "track:<id>", which follows
-- its file; never a path.

-- position: the playlist's place among the playlists, 0 to n-1.
CREATE TABLE playlist (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    position INTEGER NOT NULL,
    created INTEGER NOT NULL -- UTC ticks
);

-- A playlist holds tracks only, and may hold one track more than once.
-- position: the entry's place in its playlist, 0 to n-1.
CREATE TABLE playlist_item (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    playlist_id INTEGER NOT NULL REFERENCES playlist (id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    title TEXT NOT NULL,
    artist TEXT,
    album TEXT,
    duration_ms INTEGER
);
CREATE INDEX playlist_item_playlist ON playlist_item (playlist_id, position);

-- A track or a station, each at most once. A station's snapshot is its name,
-- in title.
CREATE TABLE favorite (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('track', 'station')),
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    title TEXT NOT NULL,
    artist TEXT,
    album TEXT,
    duration_ms INTEGER,
    added INTEGER NOT NULL, -- UTC ticks
    UNIQUE (kind, provider_id, provider_item_id)
);
