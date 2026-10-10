CREATE TABLE conversation_memory (
    chat_id uuid PRIMARY KEY REFERENCES chats(id) ON DELETE CASCADE,
    revision integer NOT NULL CHECK (revision > 0),
    through_position bigint NOT NULL CHECK (through_position > 0),
    items jsonb NOT NULL CHECK (jsonb_typeof(items) = 'array'),
    updated_at timestamptz NOT NULL DEFAULT now()
);
