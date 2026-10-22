-- The media browser (phase 14): what a library is browsed by, per track.
-- year is the track's own (an album's year stays on album); added is when
-- the indexer first recorded the track, in UTC ticks.
ALTER TABLE track ADD COLUMN genre TEXT COLLATE NOCASE;
ALTER TABLE track ADD COLUMN composer TEXT COLLATE NOCASE;
ALTER TABLE track ADD COLUMN year INTEGER;
ALTER TABLE track ADD COLUMN added INTEGER;
CREATE INDEX track_genre ON track (genre);
CREATE INDEX track_composer ON track (composer);
CREATE INDEX track_year ON track (year);

-- A track indexed before this was added when its file was last written, as
-- far as anyone can tell; its year is its album's until the next pass reads
-- its own.
UPDATE track SET
    added = (SELECT f.modified FROM library_file f WHERE f.track_id = track.id),
    year = (SELECT al.year FROM album al WHERE al.id = track.album_id);

-- Which tags the file's row was read with: 1 before genre and composer were
-- read, 2 since. The next pass probes a file below the current version again,
-- as if it had changed, and its row and track id stay; no rescan is forced.
ALTER TABLE library_file ADD COLUMN tags INTEGER NOT NULL DEFAULT 1;
