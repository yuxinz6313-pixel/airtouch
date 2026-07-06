CREATE TABLE IF NOT EXISTS device_config (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  user_id TEXT NOT NULL,
  config_version INTEGER NOT NULL,
  config_json TEXT NOT NULL,
  created_at TEXT NOT NULL,
  applied_version INTEGER,
  applied_at TEXT
);

CREATE INDEX IF NOT EXISTS idx_device_config_latest
ON device_config(device_id, user_id, config_version DESC);

CREATE INDEX IF NOT EXISTS idx_device_config_created
ON device_config(created_at);
