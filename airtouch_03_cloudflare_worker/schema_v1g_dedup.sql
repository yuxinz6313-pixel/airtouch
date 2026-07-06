CREATE UNIQUE INDEX IF NOT EXISTS idx_records_unique_device_user_type_rid
ON records(device_id, user_id, type, rid);
