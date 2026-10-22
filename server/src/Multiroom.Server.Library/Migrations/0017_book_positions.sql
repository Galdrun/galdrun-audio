-- Where a listener stopped in each book (phase 33), always saved, house-wide:
-- one row per book reference (the group's source, "local:book:3"), the part
-- (its index and reference) and the offset in it, the position in the whole
-- book and its length where known, and whether it was played to its end.
-- A title and artist are kept for a list. The newest
-- BookPositionStore.MaxRows rows are kept.
CREATE TABLE book_position (
    reference TEXT PRIMARY KEY,
    title TEXT NOT NULL,
    artist TEXT,
    part INTEGER NOT NULL,
    part_reference TEXT,
    offset_ms INTEGER NOT NULL,
    position_ms INTEGER,
    duration_ms INTEGER,
    finished INTEGER NOT NULL,
    updated INTEGER NOT NULL -- UTC ticks
);

CREATE INDEX book_position_updated ON book_position (updated);
