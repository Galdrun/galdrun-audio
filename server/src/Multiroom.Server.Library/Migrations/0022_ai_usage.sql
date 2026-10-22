-- What the AI cost, per server-local day, per origin (proposals, autoFill,
-- agent, manual, other), provider and model: one row each, added to by every
-- call sent to a vendor. Kept without limit: a row is per day, not per call.
CREATE TABLE ai_usage (
    day TEXT NOT NULL, -- yyyy-MM-dd
    origin TEXT NOT NULL,
    provider TEXT NOT NULL,
    model TEXT NOT NULL,
    calls INTEGER NOT NULL,
    failures INTEGER NOT NULL,
    input_tokens INTEGER NOT NULL,
    output_tokens INTEGER NOT NULL,
    reasoning_tokens INTEGER NOT NULL,
    web_searches INTEGER NOT NULL,
    PRIMARY KEY (day, origin, provider, model)
) WITHOUT ROWID;
