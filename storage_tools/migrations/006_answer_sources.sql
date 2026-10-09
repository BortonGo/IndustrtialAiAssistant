-- Immutable evidence selected for this question, separate from the current index.
ALTER TABLE messages ADD COLUMN source_references jsonb NOT NULL DEFAULT '[]'::jsonb
    CHECK (jsonb_typeof(source_references) = 'array');
