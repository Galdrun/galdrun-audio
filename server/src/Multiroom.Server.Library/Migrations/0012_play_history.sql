-- What the house played (phase 26): one row per track when it became audible
-- in a zone, newest last, as a reference plus the track as JSON when it
-- played; never a foreign key into track. Kept to the newest
-- PlayHistoryStore.MaxRows rows. AutoFill avoids what played lately, and the
-- API answers it as "last played".
CREATE TABLE play_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    zone TEXT NOT NULL,
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    snapshot TEXT NOT NULL,
    played INTEGER NOT NULL -- UTC ticks
);

CREATE INDEX play_history_zone ON play_history (zone, id);
