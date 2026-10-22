-- A song liked as a radio played it (provider radio-metadata), found later
-- among the providers that stream tracks (RadioLikeFinder): the FoundTrack
-- as JSON, null until then. The like itself stays as it was.
ALTER TABLE liked ADD COLUMN found TEXT;
