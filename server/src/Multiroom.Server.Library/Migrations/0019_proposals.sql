-- AI proposals (phase 38): what the server proposes to hear next, per scope
-- (user, '' the household), as a stored result every UI reads. A run is one
-- asking of the AI; its proposals are references plus snapshots, never
-- foreign keys into track.
CREATE TABLE proposal_run (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user TEXT NOT NULL,
    triggered_by TEXT NOT NULL CHECK (triggered_by IN ('manual', 'background')),
    started INTEGER NOT NULL, -- UTC ticks
    finished INTEGER,         -- UTC ticks
    state TEXT NOT NULL CHECK (state IN ('running', 'ok', 'partial', 'failed')),
    error TEXT,
    usage TEXT,               -- AiUsage as JSON
    not_found INTEGER NOT NULL DEFAULT 0,
    fingerprint TEXT NOT NULL -- a hash over the scope's marks when it started
);
CREATE INDEX proposal_run_user ON proposal_run (user, id);

-- kind: track, album, artist or audiobook; item_key: the item as one song, album
-- or artist across providers (SongResolver's folding), so another provider's
-- copy is avoided too.
CREATE TABLE proposal (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER NOT NULL REFERENCES proposal_run (id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK (kind IN ('track', 'album', 'artist', 'audiobook')),
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    item_key TEXT NOT NULL,
    snapshot TEXT NOT NULL,
    reason TEXT,
    rank INTEGER NOT NULL
);
CREATE INDEX proposal_run_rank ON proposal (run_id, kind, rank);

-- What a scope dismissed: never proposed to it again. The newest
-- ProposalStore.MaxDismissed per scope are kept.
CREATE TABLE proposal_dismissed (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user TEXT NOT NULL,
    kind TEXT NOT NULL,
    provider_id TEXT NOT NULL,
    provider_item_id TEXT NOT NULL,
    item_key TEXT NOT NULL,
    dismissed INTEGER NOT NULL -- UTC ticks
);
CREATE INDEX proposal_dismissed_user ON proposal_dismissed (user, id);
