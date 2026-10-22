-- What an item is (phase 30): the classifier's verdict per track
-- (media_type 'music' or 'audiobook', null when nothing says) and the rule
-- that gave it (media_type_source 'folder', 'tag', 'extension' or 'genre').
-- media_kind is the MP4 `stik` the indexer read (tags version 4), kept so a
-- folder whose declared type changes is classified again without a probe.
ALTER TABLE track ADD COLUMN media_kind INTEGER;
ALTER TABLE track ADD COLUMN media_type TEXT;
ALTER TABLE track ADD COLUMN media_type_source TEXT;
CREATE INDEX track_media_type ON track (media_type);

-- The snapshot keeps the type, as it keeps the format (0009).
ALTER TABLE playlist_item ADD COLUMN media_type TEXT;
ALTER TABLE favorite ADD COLUMN media_type TEXT;

-- An API folder's declared type: 'music', 'audiobook', or null (the tags
-- decide).
ALTER TABLE library_folder ADD COLUMN media_type TEXT;
