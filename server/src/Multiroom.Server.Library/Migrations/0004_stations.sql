-- Radio stations (phase 9): the webradio provider's entries, added by hand
-- or imported from M3U and PLS files. A station's provider item id is
-- "station:<id>". stream_url is where the relay connects upstream; the core
-- never sees it, because it fetches the media endpoint. Adding a stream URL
-- that is already here answers the station that has it.
CREATE TABLE station (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    stream_url TEXT NOT NULL UNIQUE,
    homepage TEXT,
    genre TEXT,
    logo TEXT,
    added INTEGER NOT NULL -- UTC ticks
);

-- rowid is station.id. Maintained by the station store beside station.
CREATE VIRTUAL TABLE station_search USING fts5 (name, genre, tokenize = 'unicode61 remove_diacritics 2');
