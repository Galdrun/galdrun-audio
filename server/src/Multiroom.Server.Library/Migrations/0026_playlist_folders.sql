-- Folders of playlists: a tree, a folder in at most one other (parent_id null
-- at the top). No two folders in one place share a name, in any case
-- (PlaylistStore checks it: a unique index cannot treat null parents as equal). A playlist lies in one folder or at the top (folder_id null);
-- its position stays its place among all the playlists. A folder's removal
-- removes everything within it, playlists too (PlaylistStore.DeleteFolder).
CREATE TABLE playlist_folder (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    parent_id INTEGER REFERENCES playlist_folder (id) ON DELETE SET NULL,
    created INTEGER NOT NULL -- UTC ticks
);

ALTER TABLE playlist ADD COLUMN folder_id INTEGER REFERENCES playlist_folder (id) ON DELETE SET NULL;
