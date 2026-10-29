-- A playlist entry may be a station: its row is the station's
-- provider-scoped identity and its name, in title, as a favourite station's.
-- It plays until skipped. The kind's CHECK changes, so the table is rebuilt
-- with its ids kept (playlist_item_part refers to them); its indexes go with
-- the old table and are made again.
CREATE TABLE playlist_item_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    playlist_id INTEGER NOT NULL REFERENCES playlist (id) ON DELETE CASCADE,
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
    media_type TEXT,
    kind TEXT NOT NULL DEFAULT 'track' CHECK (kind IN ('track', 'group', 'station'))
);
INSERT INTO playlist_item_new (id, playlist_id, position, provider_id, provider_item_id, title, artist, album,
    duration_ms, codec, sample_rate, bit_depth, channels, bitrate, media_type, kind)
SELECT id, playlist_id, position, provider_id, provider_item_id, title, artist, album,
    duration_ms, codec, sample_rate, bit_depth, channels, bitrate, media_type, kind
FROM playlist_item;
DROP TABLE playlist_item;
ALTER TABLE playlist_item_new RENAME TO playlist_item;
CREATE INDEX playlist_item_playlist ON playlist_item (playlist_id, position);
CREATE INDEX playlist_item_ref ON playlist_item (provider_id, provider_item_id);
