CREATE TABLE document_imports (
    id uuid PRIMARY KEY,
    chat_id uuid NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    document_id uuid NOT NULL,
    source_path text NOT NULL,
    source_key text NOT NULL,
    source_sha256 text NOT NULL,
    profile_id text NOT NULL REFERENCES embedding_profiles(id),
    status text NOT NULL CHECK (status IN ('queued','running','paused','failed','complete','cancelled')),
    progress text NOT NULL DEFAULT '',
    error text NOT NULL DEFAULT '',
    checkpoint jsonb NOT NULL DEFAULT '{}' CHECK (jsonb_typeof(checkpoint)='object'),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE UNIQUE INDEX one_pending_import_per_file ON document_imports(chat_id,source_key)
    WHERE status IN ('queued','running','paused','failed');
CREATE INDEX document_imports_queue ON document_imports(created_at,id) WHERE status='queued';
CREATE TABLE document_import_chunks (
    import_id uuid NOT NULL REFERENCES document_imports(id) ON DELETE CASCADE,
    position integer NOT NULL CHECK (position >= 0),
    payload jsonb NOT NULL,
    PRIMARY KEY (import_id,position)
);
