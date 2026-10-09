-- Native text remains searchable while vision is running or paused.
ALTER TABLE documents ADD COLUMN pending_images integer NOT NULL DEFAULT 0 CHECK (pending_images >= 0);
ALTER TABLE document_imports ADD COLUMN native_done boolean NOT NULL DEFAULT false;
