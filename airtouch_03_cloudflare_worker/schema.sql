CREATE TABLE IF NOT EXISTS records (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  type TEXT NOT NULL,
  rid INTEGER NOT NULL,
  source TEXT,
  payload_json TEXT NOT NULL,
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_records_device_user_time
ON records(device_id, user_id, created_at);

CREATE INDEX IF NOT EXISTS idx_records_type_time
ON records(type, created_at);
