CREATE TABLE message_attachments (
    message_id uuid NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    position integer NOT NULL CHECK (position >= 0 AND position < 4),
    file_name text NOT NULL,
    source_path text NOT NULL,
    document_id uuid REFERENCES documents(id) ON DELETE SET NULL,
    preview_image text NOT NULL DEFAULT '',
    PRIMARY KEY (message_id, position)
);
CREATE INDEX message_attachments_document ON message_attachments(document_id);
