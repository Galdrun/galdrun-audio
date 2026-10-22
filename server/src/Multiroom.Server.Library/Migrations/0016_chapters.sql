-- Chapter marks inside one file (phase 32): an audiobook bought or converted
-- as one .m4b or one long MP3. Read at probe time (MP4 chapter track or
-- Nero chpl, ID3v2 CHAP), at most 1000 per file, each its title and where it
-- starts; its end is the next one's start, or the track's length. They go
-- with their track.
CREATE TABLE track_chapter (
    track_id INTEGER NOT NULL REFERENCES track (id) ON DELETE CASCADE,
    idx INTEGER NOT NULL,
    title TEXT NOT NULL,
    start_ms INTEGER NOT NULL,
    PRIMARY KEY (track_id, idx)
);

-- A stored book's part may be a chapter of its file: its title and where it
-- starts and ends in that file, so it plays as it was saved.
ALTER TABLE playlist_item_part ADD COLUMN chapter TEXT;
ALTER TABLE playlist_item_part ADD COLUMN start_ms INTEGER;
ALTER TABLE playlist_item_part ADD COLUMN end_ms INTEGER;
ALTER TABLE favorite_part ADD COLUMN chapter TEXT;
ALTER TABLE favorite_part ADD COLUMN start_ms INTEGER;
ALTER TABLE favorite_part ADD COLUMN end_ms INTEGER;
