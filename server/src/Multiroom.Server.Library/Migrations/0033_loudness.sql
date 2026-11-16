-- A track's integrated loudness in LUFS (EBU R128), from its ReplayGain or
-- R128 tags; null when the file has none. The indexer reads it into track
-- (tags version 7, so the next pass probes every older file again). Playback
-- turns it into the track's normalization gain (ZoneSession), which a room
-- plays only where its normalization is on (MATP section 59).
ALTER TABLE track ADD COLUMN loudness_lufs REAL;
