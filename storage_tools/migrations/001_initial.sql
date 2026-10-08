CREATE TABLE chats (
    id uuid PRIMARY KEY,
    title text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE messages (
    id uuid PRIMARY KEY,
    chat_id uuid NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    position bigint GENERATED ALWAYS AS IDENTITY,
    role text NOT NULL CHECK (role IN ('user', 'assistant')),
    text text NOT NULL,
    status text NOT NULL CHECK (status IN ('pending', 'complete', 'failed')),
    error text NOT NULL DEFAULT '',
    reply_to uuid,
    context_snapshot text NOT NULL DEFAULT '',
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (chat_id, id),
    UNIQUE (chat_id, position),
    FOREIGN KEY (chat_id, reply_to) REFERENCES messages(chat_id, id),
    CHECK ((role = 'user' AND reply_to IS NULL) OR
           (role = 'assistant' AND reply_to IS NOT NULL AND status = 'complete'))
);
CREATE UNIQUE INDEX one_answer_per_question ON messages(reply_to) WHERE role = 'assistant';
CREATE TABLE embedding_profiles (
    id text PRIMARY KEY,
    model_sha256 text NOT NULL,
    preprocessing_version text NOT NULL,
    dimension integer NOT NULL CHECK (dimension > 0)
);
CREATE TABLE documents (
    id uuid PRIMARY KEY,
    chat_id uuid NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    source_path text NOT NULL,
    source_key text NOT NULL,
    content_sha256 text NOT NULL,
    extracted_text text NOT NULL,
    warnings jsonb NOT NULL CHECK (jsonb_typeof(warnings) = 'array'),
    pipeline_version text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (chat_id, source_key)
);
CREATE TABLE document_blocks (
    document_id uuid NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
    position integer NOT NULL CHECK (position >= 0),
    kind text NOT NULL CHECK (kind IN ('text', 'table', 'image')),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload) = 'object'),
    PRIMARY KEY (document_id, position)
);
CREATE TABLE chunks (
    document_id uuid NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
    position integer NOT NULL CHECK (position >= 0),
    text text NOT NULL,
    start_offset integer NOT NULL CHECK (start_offset >= 0),
    metadata jsonb NOT NULL CHECK (jsonb_typeof(metadata) = 'object'),
    profile_id text NOT NULL REFERENCES embedding_profiles(id),
    embedding double precision[] NOT NULL,
    PRIMARY KEY (document_id, position),
    CHECK (array_ndims(embedding) = 1 AND cardinality(embedding) > 0)
);
CREATE INDEX chunks_profile ON chunks(profile_id);
CREATE TABLE app_settings (key text PRIMARY KEY, value text NOT NULL);
