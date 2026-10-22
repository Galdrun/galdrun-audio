-- A favourite may be a group (a book, phases 31 and the owner's later ask):
-- its row is the node it was expanded from (provider_id, provider_item_id),
-- its title, artist, whole duration and media type, and its parts are
-- favorite_part rows, a reference plus a snapshot each, deleted with it.
-- The kind's CHECK changes, so the table is rebuilt with its ids kept.
CREATE TABLE favorite_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('track', 'station', 'group')),
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
    UNIQUE (kind, provider_id, provider_item_id)
);
INSERT INTO favorite_new (id, kind, provider_id, provider_item_id, title, artist, album, duration_ms, added,
    codec, sample_rate, bit_depth, channels, bitrate, media_type)
SELECT id, kind, provider_id, provider_item_id, title, artist, album, duration_ms, added,
    codec, sample_rate, bit_depth, channels, bitrate, media_type
FROM favorite;
DROP TABLE favorite;
ALTER TABLE favorite_new RENAME TO favorite;

CREATE TABLE favorite_part (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    favorite_id INTEGER NOT NULL REFERENCES favorite (id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    title TEXT NOT NULL,
    artist TEXT,
    album TEXT,
    duration_ms INTEGER,
    codec TEXT,
    sample_rate INTEGER,
    bit_depth INTEGER,
    channels INTEGER,
    bitrate INTEGER,
    media_type TEXT
);
CREATE INDEX favorite_part_favorite ON favorite_part (favorite_id, position);
