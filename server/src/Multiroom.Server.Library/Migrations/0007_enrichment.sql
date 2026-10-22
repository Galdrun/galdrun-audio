-- The add-in surface (phase 12): what an enrichment add-in derived for a
-- library track, as one JSON value, or why it could not. A track is asked
-- about once per add-in: the row goes with its track, and the indexer
-- deletes it when the track's file changes, so the track is asked again.
CREATE TABLE enrichment (
    track_id INTEGER NOT NULL REFERENCES track (id) ON DELETE CASCADE,
    addin TEXT NOT NULL,
    data TEXT,     -- JSON; null when it failed
    failure TEXT,  -- null when it answered
    enriched INTEGER NOT NULL, -- UTC ticks
    PRIMARY KEY (addin, track_id)
);
CREATE INDEX enrichment_track ON enrichment (track_id);
