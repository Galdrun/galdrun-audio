-- A track's id is what a saved playlist holds (phase 7), so it must survive a
-- file moving and must never be given to another track.

-- AUTOINCREMENT: a deleted track's id is never reused. SQLite cannot add it to
-- a table, so track is rebuilt with its ids kept (the runner turns foreign
-- keys off while it migrates and checks them before it commits).
CREATE TABLE track_new (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    artist_id INTEGER REFERENCES artist (id),
    album_id INTEGER REFERENCES album (id),
    disc INTEGER,
    number INTEGER,
    duration_ms INTEGER
);
INSERT INTO track_new (id, title, artist_id, album_id, disc, number, duration_ms)
    SELECT id, title, artist_id, album_id, disc, number, duration_ms FROM track;
DROP TABLE track;
ALTER TABLE track_new RENAME TO track;
CREATE INDEX track_artist ON track (artist_id);
CREATE INDEX track_album ON track (album_id);

-- "<size>:<sha-256 of the first 64 KiB>": how a moved file is recognised.
ALTER TABLE library_file ADD COLUMN fingerprint TEXT;
CREATE INDEX library_file_fingerprint ON library_file (fingerprint);

-- UTC ticks of the pass that no longer found the file. A missing file is
-- hidden but kept, so a pass of another folder can still find it moved there.
ALTER TABLE library_file ADD COLUMN missing_since INTEGER;
