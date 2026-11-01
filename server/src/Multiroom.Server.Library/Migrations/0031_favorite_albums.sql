-- A favourite may be an album: its row is the album's provider-scoped
-- identity, its title and artist; it is queued as its provider's node.
-- The kind's CHECK changes, so the table is rebuilt with its ids kept
-- (favorite_part refers to them).
CREATE TABLE favorite_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('track', 'station', 'group', 'album')),
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    title TEXT NOT NULL,
    artist TEXT,
    album TEXT,
    duration_ms INTEGER,
    added INTEGER NOT NULL, -- UTC ticks
    codec TEXT,
    sample_rate INTEGER,
    bit_depth INTEGER,
    channels INTEGER,
    bitrate INTEGER,
    media_type TEXT,
    user TEXT NOT NULL DEFAULT '',
    UNIQUE (kind, provider_id, provider_item_id, user)
);
INSERT INTO favorite_new (id, kind, provider_id, provider_item_id, title, artist, album, duration_ms, added,
    codec, sample_rate, bit_depth, channels, bitrate, media_type, user)
SELECT id, kind, provider_id, provider_item_id, title, artist, album, duration_ms, added,
    codec, sample_rate, bit_depth, channels, bitrate, media_type, user
FROM favorite;
DROP TABLE favorite;
ALTER TABLE favorite_new RENAME TO favorite;
