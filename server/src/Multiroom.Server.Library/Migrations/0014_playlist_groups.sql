-- A playlist entry may be a group (phase 31): a book kept as one entry.
-- Its row is the node it was expanded from (provider_id, provider_item_id:
-- "book:3"), its title, artist, whole duration and media type; its parts are
-- playlist_item_part rows, a reference plus a snapshot each as an entry is,
-- stored rather than expanded again, so a book plays in the order it was
-- saved. They go with their entry.
ALTER TABLE playlist_item ADD COLUMN kind TEXT NOT NULL DEFAULT 'track' CHECK (kind IN ('track', 'group'));

CREATE TABLE playlist_item_part (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id INTEGER NOT NULL REFERENCES playlist_item (id) ON DELETE CASCADE,
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
CREATE INDEX playlist_item_part_item ON playlist_item_part (item_id, position);
