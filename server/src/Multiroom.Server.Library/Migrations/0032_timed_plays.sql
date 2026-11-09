-- Timed plays: a playlist started and stopped in some rooms by a weekly
-- schedule (TimedPlayStore). The definition is the TimedPlay record as JSON;
-- run is the scheduler's own record of a start it made (null when none),
-- resume where the last run stopped, both JSON, so a restart keeps them.
CREATE TABLE timed_play (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    definition TEXT NOT NULL,
    run TEXT,
    resume TEXT
);
