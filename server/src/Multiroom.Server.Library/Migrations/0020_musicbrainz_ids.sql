-- The MusicBrainz ids Picard writes into a file's tags (tags version 6): the
-- release group, the release, the track's artist and the album's artist,
-- each the first id of its tag, lower case, or null. The artwork add-in looks
-- an album or an artist up by them instead of by name.
ALTER TABLE track ADD COLUMN mb_release_group TEXT;
ALTER TABLE track ADD COLUMN mb_release TEXT;
ALTER TABLE track ADD COLUMN mb_artist TEXT;
ALTER TABLE track ADD COLUMN mb_album_artist TEXT;
