-- Folders of radio stations, as the playlists have theirs (0026): a tree, a
-- folder in at most one other (parent_id null at the top), no two in one
-- place sharing a name in any case (FolderTree checks it). A station lies in
-- one folder or at the top (folder_id null). A folder's removal removes
-- everything within it, stations too (StationStore.DeleteFolder).
CREATE TABLE station_folder (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    parent_id INTEGER REFERENCES station_folder (id) ON DELETE SET NULL,
    created INTEGER NOT NULL -- UTC ticks
);

ALTER TABLE station ADD COLUMN folder_id INTEGER REFERENCES station_folder (id) ON DELETE SET NULL;
