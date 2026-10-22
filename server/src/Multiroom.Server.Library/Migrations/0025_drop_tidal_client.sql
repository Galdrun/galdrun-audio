-- Tidal's apps are read from the config file only (providers.tidal); the
-- one set through the API is no longer kept.
DROP TABLE IF EXISTS tidal_client;
