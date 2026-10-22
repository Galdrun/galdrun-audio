-- Library folders added through the API (phase 13), beside those of the
-- config file, which are not here: the config is the fact at startup, and
-- its folders are read-only to the API. A folder's id in the API is this
-- row's id; a config folder's is "config:<n>".
--
-- path: a local or network folder's full path; an smb folder's directory
-- inside its share ('' for the share's root). An smb folder's password is
-- not here but in <dataDirectory>/library/credentials.json, owner-only.
CREATE TABLE library_folder (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    type TEXT NOT NULL CHECK (type IN ('local', 'network', 'smb')),
    path TEXT NOT NULL,
    host TEXT,
    port INTEGER,
    share TEXT,
    user TEXT,
    domain TEXT,
    enabled INTEGER NOT NULL,
    added INTEGER NOT NULL -- UTC ticks
);
