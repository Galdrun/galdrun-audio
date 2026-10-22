-- Users (phase 36): a label a like, a favourite or a play may carry, not an
-- account. An empty user is the household, so every row before this
-- migration keeps its meaning. A name is unique without regard to case.
CREATE TABLE household_user (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL COLLATE NOCASE UNIQUE,
    added INTEGER NOT NULL -- UTC ticks
);

-- A like is unique per user: the household and one person may each like an
-- item. The kind takes 'audiobook' too (phase 37), so the table is rebuilt
-- once, with its ids kept.
CREATE TABLE liked_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('track', 'album', 'artist', 'station', 'audiobook')),
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    snapshot TEXT NOT NULL,
    added INTEGER NOT NULL, -- UTC ticks
    user TEXT NOT NULL DEFAULT '',
    UNIQUE (kind, provider_id, provider_item_id, user)
);
INSERT INTO liked_new (id, kind, provider_id, provider_item_id, snapshot, added)
SELECT id, kind, provider_id, provider_item_id, snapshot, added FROM liked;
DROP TABLE liked;
ALTER TABLE liked_new RENAME TO liked;

-- The album likes from before phase 37, each asked once at start whether
-- its album is a book (AudiobookLikes); a row goes once it is answered.
CREATE TABLE liked_album_check (
    like_id INTEGER PRIMARY KEY REFERENCES liked (id) ON DELETE CASCADE
);
INSERT INTO liked_album_check (like_id) SELECT id FROM liked WHERE kind = 'album';

-- The same for a favourite; its parts keep pointing at its kept id.
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
    user TEXT NOT NULL DEFAULT '',
    UNIQUE (kind, provider_id, provider_item_id, user)
);
INSERT INTO favorite_new (id, kind, provider_id, provider_item_id, title, artist, album, duration_ms, added,
    codec, sample_rate, bit_depth, channels, bitrate, media_type)
SELECT id, kind, provider_id, provider_item_id, title, artist, album, duration_ms, added,
    codec, sample_rate, bit_depth, channels, bitrate, media_type
FROM favorite;
DROP TABLE favorite;
ALTER TABLE favorite_new RENAME TO favorite;

-- Who was listening in the zone when the track became audible.
ALTER TABLE play_history ADD COLUMN user TEXT NOT NULL DEFAULT '';
