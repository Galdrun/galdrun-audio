-- The library: the local provider's folders, as the indexer last saw them.
-- Every row belongs to the local provider, so no row carries a provider id.
-- Foreign keys are enforced: Microsoft.Data.Sqlite turns them on.

CREATE TABLE artist (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL COLLATE NOCASE UNIQUE
);

CREATE TABLE album (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL COLLATE NOCASE,
    artist_id INTEGER REFERENCES artist (id),
    year INTEGER
);
CREATE UNIQUE INDEX album_key ON album (title, IFNULL(artist_id, 0));
CREATE INDEX album_artist ON album (artist_id);

CREATE TABLE track (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    artist_id INTEGER REFERENCES artist (id),
    album_id INTEGER REFERENCES album (id),
    disc INTEGER,
    number INTEGER,
    duration_ms INTEGER
);
CREATE INDEX track_artist ON track (artist_id);
CREATE INDEX track_album ON track (album_id);

-- One row per audio file seen. A file that did not probe has an error and no
-- track, and is not probed again until its size or write time changes.
CREATE TABLE library_file (
    path TEXT PRIMARY KEY,
    folder TEXT NOT NULL,
    directory TEXT NOT NULL,
    size INTEGER NOT NULL,
    modified INTEGER NOT NULL, -- UTC ticks
    track_id INTEGER UNIQUE REFERENCES track (id) ON DELETE SET NULL,
    error TEXT
);
CREATE INDEX library_file_folder ON library_file (folder);
CREATE INDEX library_file_directory ON library_file (directory);

-- rowid is track.id. Maintained by the indexer beside track.
CREATE VIRTUAL TABLE track_search USING fts5 (title, artist, album, tokenize = 'unicode61 remove_diacritics 2');
