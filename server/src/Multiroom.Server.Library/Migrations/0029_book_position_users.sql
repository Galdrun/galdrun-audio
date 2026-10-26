-- Book positions per listener (phase 36's users): one row per book and user,
-- '' the household, so each person resumes where they stopped. Every row
-- before this migration is the household's.
CREATE TABLE book_position_new (
    reference TEXT NOT NULL,
    user TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL,
    artist TEXT,
    part INTEGER NOT NULL,
    part_reference TEXT,
    offset_ms INTEGER NOT NULL,
    position_ms INTEGER,
    duration_ms INTEGER,
    finished INTEGER NOT NULL,
    updated INTEGER NOT NULL, -- UTC ticks
    PRIMARY KEY (reference, user)
);
INSERT INTO book_position_new (reference, title, artist, part, part_reference, offset_ms, position_ms, duration_ms,
    finished, updated)
SELECT reference, title, artist, part, part_reference, offset_ms, position_ms, duration_ms, finished, updated
FROM book_position;
DROP TABLE book_position;
ALTER TABLE book_position_new RENAME TO book_position;

CREATE INDEX book_position_updated ON book_position (updated);
