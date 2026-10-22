-- What a track's audio is (AudioFormat): the codec, the sample rate in Hz,
-- the bits per sample, the channels and the bitrate in kbit/s; null when not
-- known. The indexer reads it into track (tags version 3, so the next pass
-- probes every older file again); playlists and favourites keep it in their
-- snapshot, as given when the track was added.
ALTER TABLE track ADD COLUMN codec TEXT;
ALTER TABLE track ADD COLUMN sample_rate INTEGER;
ALTER TABLE track ADD COLUMN bit_depth INTEGER;
ALTER TABLE track ADD COLUMN channels INTEGER;
ALTER TABLE track ADD COLUMN bitrate INTEGER;

ALTER TABLE playlist_item ADD COLUMN codec TEXT;
ALTER TABLE playlist_item ADD COLUMN sample_rate INTEGER;
ALTER TABLE playlist_item ADD COLUMN bit_depth INTEGER;
ALTER TABLE playlist_item ADD COLUMN channels INTEGER;
ALTER TABLE playlist_item ADD COLUMN bitrate INTEGER;

ALTER TABLE favorite ADD COLUMN codec TEXT;
ALTER TABLE favorite ADD COLUMN sample_rate INTEGER;
ALTER TABLE favorite ADD COLUMN bit_depth INTEGER;
ALTER TABLE favorite ADD COLUMN channels INTEGER;
ALTER TABLE favorite ADD COLUMN bitrate INTEGER;
