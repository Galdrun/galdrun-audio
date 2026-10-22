-- The server's name and the images of the server and its rooms, set through
-- the API (server, server/image, rooms/{client}/image). The name is one row,
-- GALDRUN-SERVER-#### with four random digits until it is set.
CREATE TABLE server_profile (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    name TEXT NOT NULL
);

INSERT INTO server_profile (id, name) VALUES (1, printf('GALDRUN-SERVER-%04d', abs(random() % 10000)));

-- owner is 'server' or 'room:<client>'; the bytes as uploaded, their type
-- read from them.
CREATE TABLE profile_image (
    owner TEXT PRIMARY KEY,
    content_type TEXT NOT NULL,
    bytes BLOB NOT NULL,
    updated INTEGER NOT NULL
);
