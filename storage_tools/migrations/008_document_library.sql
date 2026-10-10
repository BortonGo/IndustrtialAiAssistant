-- Chat documents and immutable library snapshots share the same content schema.
-- Only library snapshots have no chat owner; existing chat rows are unchanged.
ALTER TABLE documents ALTER COLUMN chat_id DROP NOT NULL;
CREATE TABLE document_library (
    id uuid PRIMARY KEY REFERENCES documents(id) ON DELETE CASCADE,
    fingerprint text NOT NULL UNIQUE,
    profile_id text NOT NULL REFERENCES embedding_profiles(id),
    created_at timestamptz NOT NULL DEFAULT now()
);
