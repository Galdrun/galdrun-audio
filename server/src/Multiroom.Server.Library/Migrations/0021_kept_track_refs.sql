-- Search answers the tracks the house keeps first (a playlist's, a
-- favourite's, a liked one): each is found by its reference. favorite and
-- liked are indexed by theirs already (UNIQUE).
CREATE INDEX playlist_item_ref ON playlist_item (provider_id, provider_item_id);
CREATE INDEX playlist_item_part_ref ON playlist_item_part (provider_id, provider_item_id);
CREATE INDEX favorite_part_ref ON favorite_part (provider_id, provider_item_id);
