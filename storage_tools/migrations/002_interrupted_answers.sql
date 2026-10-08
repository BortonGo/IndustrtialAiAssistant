-- Completed answers and already displayed fragments of interrupted answers.
-- A failed pair is retained for the user, but question_history excludes it.
ALTER TABLE messages DROP CONSTRAINT messages_check;
ALTER TABLE messages ADD CONSTRAINT messages_role_reply_status CHECK (
    (role = 'user' AND reply_to IS NULL) OR
    (role = 'assistant' AND reply_to IS NOT NULL AND status IN ('complete', 'failed'))
);
