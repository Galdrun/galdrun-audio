-- The Tidal app the account is linked through, set through the API (PUT
-- providers/tidal/client). One row or none; with none, the config's
-- providers.tidal.clientId and clientSecret are used.
CREATE TABLE tidal_client (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    client_id TEXT NOT NULL,
    client_secret TEXT
);
