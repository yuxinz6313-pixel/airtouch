const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, X-AirTouch-Token"
};

function jsonResponse(data, status = 200) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      ...CORS_HEADERS
    }
  });
}

function htmlResponse(html, status = 200) {
  return new Response(html, {
    status,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      "Cache-Control": "no-store",
      ...CORS_HEADERS
    }
  });
}

function textResponse(text, status = 200) {
  return new Response(text, {
    status,
    headers: {
      "Content-Type": "text/plain; charset=utf-8",
      ...CORS_HEADERS
    }
  });
}

async function handleRecordPost(request, env) {
  const token = request.headers.get("X-AirTouch-Token") || "";
  if (token !== env.AIRTOUCH_TOKEN) {
    return jsonResponse({ ok: false, error: "UNAUTHORIZED" }, 401);
  }

  let data;
  try {
    data = await request.json();
  } catch (e) {
    return jsonResponse({ ok: false, error: "BAD_JSON" }, 400);
  }

  const device_id = String(data.device_id || "");
  const user_id = String(data.user_id || "");
  const type = String(data.type || "");
  const rid = Number(data.rid || 0);
  const source = String(data.source || "");
  const created_at = new Date().toISOString();
  const payload_json = JSON.stringify(data);

  if (!device_id || !user_id || !type || !rid) {
    return jsonResponse({
      ok: false,
      error: "MISSING_REQUIRED_FIELDS",
      required: ["device_id", "user_id", "type", "rid"]
    }, 400);
  }

  await env.airtouch_db.prepare(
    `INSERT OR IGNORE INTO records
     (device_id, user_id, type, rid, source, payload_json, created_at)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).bind(
    device_id,
    user_id,
    type,
    rid,
    source,
    payload_json,
    created_at
  ).run();

  const existing = await env.airtouch_db.prepare(
    `SELECT id, created_at
     FROM records
     WHERE device_id = ? AND user_id = ? AND type = ? AND rid = ?
     LIMIT 1`
  ).bind(device_id, user_id, type, rid).first();

  return jsonResponse({
    ok: true,
    saved: true,
    id: existing ? existing.id : null,
    type,
    rid,
    created_at: existing ? existing.created_at : created_at
  });
}

async function getLatestRecords(env, limit = 50) {
  const safeLimit = Math.max(1, Math.min(Number(limit) || 50, 100));

  const result = await env.airtouch_db.prepare(
    `SELECT id, device_id, user_id, type, rid, source, payload_json, created_at
     FROM records
     ORDER BY id DESC
     LIMIT ?`
  ).bind(safeLimit).all();

  return (result.results || []).map((row) => {
    let payload = {};
    try {
      payload = JSON.parse(row.payload_json);
    } catch (e) {
      payload = {};
    }

    return {
      id: row.id,
      device_id: row.device_id,
      user_id: row.user_id,
      type: row.type,
      rid: row.rid,
      source: row.source,
      created_at: row.created_at,
      payload
    };
  });
}

async function handleLatest(request, env) {
  const url = new URL(request.url);
  const limit = url.searchParams.get("limit") || 50;
  const records = await getLatestRecords(env, limit);

  return jsonResponse({
    ok: true,
    count: records.length,
    records
  });
}

async function handleSummary(env) {
  const total = await env.airtouch_db.prepare(
    `SELECT COUNT(*) AS count FROM records`
  ).first();

  const byType = await env.airtouch_db.prepare(
    `SELECT type, COUNT(*) AS count
     FROM records
     GROUP BY type
     ORDER BY type`
  ).all();

  const latestStar = await env.airtouch_db.prepare(
    `SELECT payload_json, created_at
     FROM records
     WHERE type = 'star'
     ORDER BY id DESC
     LIMIT 1`
  ).first();

  const latestColor = await env.airtouch_db.prepare(
    `SELECT payload_json, created_at
     FROM records
     WHERE type = 'color_go'
     ORDER BY id DESC
     LIMIT 1`
  ).first();

  function parseLatest(row) {
    if (!row) return null;
    try {
      return {
        created_at: row.created_at,
        payload: JSON.parse(row.payload_json)
      };
    } catch (e) {
      return {
        created_at: row.created_at,
        payload: {}
      };
    }
  }

  return jsonResponse({
    ok: true,
    total_records: total ? total.count : 0,
    by_type: byType.results || [],
    latest_star: parseLatest(latestStar),
    latest_color_go: parseLatest(latestColor)
  });
}


// -----------------------------------------------------------------------------
// Cloud-SD v2a records query API
//
// Adds:
//   GET /api/airtouch/records?type=star|color_go|all&range=day|week|month|year|all
//
// Notes:
//   - Does not change POST /api/airtouch/records.
//   - Does not change D1 schema.
//   - Uses created_at ISO string for time filtering.
// -----------------------------------------------------------------------------

const AIRTOUCH_RANGE_MS_V2A = {
  day: 24 * 60 * 60 * 1000,
  week: 7 * 24 * 60 * 60 * 1000,
  month: 30 * 24 * 60 * 60 * 1000,
  year: 365 * 24 * 60 * 60 * 1000
};

function normalizeRecordTypeV2a(value) {
  const t = String(value || "all").trim();

  if (t === "star") {
    return "star";
  }

  if (t === "color" || t === "color_go" || t === "colorgo") {
    return "color_go";
  }

  return "all";
}

function normalizeRangeV2a(value) {
  const r = String(value || "week").trim();

  if (r === "day" || r === "week" || r === "month" || r === "year" || r === "all") {
    return r;
  }

  return "week";
}

function safeLimitV2a(value, fallback = 200, min = 1, max = 1000) {
  const n = Number.parseInt(value || "", 10);

  if (!Number.isFinite(n)) {
    return fallback;
  }

  return Math.max(min, Math.min(max, n));
}

function getRangeStartIsoV2a(range) {
  if (range === "all") {
    return null;
  }

  const ms = AIRTOUCH_RANGE_MS_V2A[range] || AIRTOUCH_RANGE_MS_V2A.week;
  return new Date(Date.now() - ms).toISOString();
}

function parsePayloadJsonV2a(value) {
  if (!value) {
    return {};
  }

  try {
    return JSON.parse(value);
  } catch (err) {
    return {
      _parse_error: true,
      _raw: value
    };
  }
}

function normalizeRecordRowV2a(row) {
  const payload = parsePayloadJsonV2a(row.payload_json);

  return {
    id: row.id,
    device_id: row.device_id,
    user_id: row.user_id,
    type: row.type,
    rid: row.rid,
    source: row.source,
    created_at: row.created_at,
    payload
  };
}

async function handleRecordsGet(request, env) {
  const url = new URL(request.url);

  const type = normalizeRecordTypeV2a(url.searchParams.get("type"));
  const range = normalizeRangeV2a(url.searchParams.get("range"));
  const limit = safeLimitV2a(url.searchParams.get("limit"), 200, 1, 1000);
  const from = url.searchParams.get("from") || getRangeStartIsoV2a(range);
  const to = url.searchParams.get("to") || null;

  let sql = `
    SELECT id, device_id, user_id, type, rid, source, payload_json, created_at
    FROM records
    WHERE 1 = 1
  `;

  const binds = [];

  if (type !== "all") {
    sql += ` AND type = ?`;
    binds.push(type);
  }

  if (from) {
    sql += ` AND created_at >= ?`;
    binds.push(from);
  }

  if (to) {
    sql += ` AND created_at <= ?`;
    binds.push(to);
  }

  sql += `
    ORDER BY created_at DESC, id DESC
    LIMIT ?
  `;
  binds.push(limit);

  const result = await env.airtouch_db.prepare(sql).bind(...binds).all();
  const records = (result.results || []).map(normalizeRecordRowV2a);

  return jsonResponse({
    ok: true,
    api: "records_v2a",
    type,
    range,
    from,
    to,
    limit,
    count: records.length,
    records
  });
}


// -----------------------------------------------------------------------------
// Cloud-SD v2b trends API
//
// Adds:
//   GET /api/airtouch/trends?type=star|color_go|all&range=day|week|month|year|all
//
// Purpose:
//   Provide Garmin-like trend data for dashboard line charts.
//   - day   : bucket by hour
//   - week  : bucket by day
//   - month : bucket by day
//   - year  : bucket by month
// -----------------------------------------------------------------------------

function trendBucketModeV2b(range) {
  if (range === "day") {
    return "hour";
  }

  if (range === "year") {
    return "month";
  }

  return "day";
}

function pad2V2b(value) {
  return String(value).padStart(2, "0");
}

function trendBucketKeyV2b(iso, range) {
  const d = new Date(iso);

  if (Number.isNaN(d.getTime())) {
    return "unknown";
  }

  const y = d.getUTCFullYear();
  const m = pad2V2b(d.getUTCMonth() + 1);
  const day = pad2V2b(d.getUTCDate());
  const h = pad2V2b(d.getUTCHours());

  if (range === "day") {
    return `${y}-${m}-${day}T${h}:00Z`;
  }

  if (range === "year") {
    return `${y}-${m}`;
  }

  return `${y}-${m}-${day}`;
}

function toNumberV2b(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function addMetricV2b(agg, name, value) {
  const n = toNumberV2b(value);

  if (n === null) {
    return;
  }

  agg._sum[name] = (agg._sum[name] || 0) + n;
  agg._count[name] = (agg._count[name] || 0) + 1;
}

function avgMetricV2b(agg, name) {
  const count = agg._count[name] || 0;

  if (count <= 0) {
    return 0;
  }

  return Math.round((agg._sum[name] / count) * 10) / 10;
}

function sumMetricV2b(agg, name) {
  return Math.round(agg._sum[name] || 0);
}

function makeTrendAggV2b(bucket) {
  return {
    bucket,
    count: 0,
    _sum: Object.create(null),
    _count: Object.create(null),
    best_hits: 0,
    best_hit_score: 0,
    best_accuracy: 0,
    best_inhibition: 0
  };
}

function getTrendAggV2b(map, bucket) {
  let agg = map.get(bucket);

  if (!agg) {
    agg = makeTrendAggV2b(bucket);
    map.set(bucket, agg);
  }

  return agg;
}

function averagePresentV2b(values) {
  const nums = values
    .map((v) => Number(v))
    .filter((v) => Number.isFinite(v));

  if (nums.length === 0) {
    return 0;
  }

  return nums.reduce((a, b) => a + b, 0) / nums.length;
}

function recordOverallScoreV2b(type, payload) {
  if (type === "star") {
    return averagePresentV2b([
      payload.hit_score,
      payload.speed_score
    ]);
  }

  if (type === "color_go") {
    return averagePresentV2b([
      payload.accuracy,
      payload.inhibition,
      payload.speed_score
    ]);
  }

  return 0;
}

function finalizeStarTrendV2b(agg) {
  return {
    bucket: agg.bucket,
    count: agg.count,
    hits_avg: avgMetricV2b(agg, "hits"),
    hit_score_avg: avgMetricV2b(agg, "hit_score"),
    speed_score_avg: avgMetricV2b(agg, "speed_score"),
    avg_ms_avg: avgMetricV2b(agg, "avg_ms"),
    fastest_ms_avg: avgMetricV2b(agg, "fastest_ms"),
    difficulty_avg: avgMetricV2b(agg, "difficulty"),
    dwell_ms_avg: avgMetricV2b(agg, "dwell_ms"),
    target_r_avg: avgMetricV2b(agg, "target_r"),
    best_hits: agg.best_hits,
    best_hit_score: agg.best_hit_score
  };
}

function finalizeColorTrendV2b(agg) {
  return {
    bucket: agg.bucket,
    count: agg.count,
    correct_total: sumMetricV2b(agg, "correct"),
    wrong_total: sumMetricV2b(agg, "wrong"),
    false_alarm_total: sumMetricV2b(agg, "false_alarm"),
    miss_total: sumMetricV2b(agg, "miss"),
    accuracy_avg: avgMetricV2b(agg, "accuracy"),
    inhibition_avg: avgMetricV2b(agg, "inhibition"),
    speed_score_avg: avgMetricV2b(agg, "speed_score"),
    avg_ms_avg: avgMetricV2b(agg, "avg_ms"),
    fastest_ms_avg: avgMetricV2b(agg, "fastest_ms"),
    difficulty_avg: avgMetricV2b(agg, "difficulty"),
    dwell_ms_avg: avgMetricV2b(agg, "dwell_ms"),
    bubble_count_avg: avgMetricV2b(agg, "bubble_count"),
    nogo_ratio_avg: avgMetricV2b(agg, "nogo_ratio"),
    best_accuracy: agg.best_accuracy,
    best_inhibition: agg.best_inhibition
  };
}

function finalizeOverallTrendV2b(agg) {
  return {
    bucket: agg.bucket,
    count: agg.count,
    overall_score_avg: avgMetricV2b(agg, "overall_score"),
    avg_ms_avg: avgMetricV2b(agg, "avg_ms"),
    training_count: agg.count
  };
}

function sortedTrendSeriesV2b(map, finalizer) {
  return Array.from(map.values())
    .sort((a, b) => String(a.bucket).localeCompare(String(b.bucket)))
    .map(finalizer);
}

async function handleTrendsGet(request, env) {
  const url = new URL(request.url);

  const type = normalizeRecordTypeV2a(url.searchParams.get("type"));
  const range = normalizeRangeV2a(url.searchParams.get("range"));
  const limit = safeLimitV2a(url.searchParams.get("limit"), 5000, 1, 5000);
  const from = url.searchParams.get("from") || getRangeStartIsoV2a(range);
  const to = url.searchParams.get("to") || null;

  let sql = `
    SELECT id, device_id, user_id, type, rid, source, payload_json, created_at
    FROM records
    WHERE 1 = 1
  `;

  const binds = [];

  if (type !== "all") {
    sql += ` AND type = ?`;
    binds.push(type);
  }

  if (from) {
    sql += ` AND created_at >= ?`;
    binds.push(from);
  }

  if (to) {
    sql += ` AND created_at <= ?`;
    binds.push(to);
  }

  sql += `
    ORDER BY created_at ASC, id ASC
    LIMIT ?
  `;
  binds.push(limit);

  const result = await env.airtouch_db.prepare(sql).bind(...binds).all();
  const rows = result.results || [];

  const starMap = new Map();
  const colorMap = new Map();
  const overallMap = new Map();

  for (const row of rows) {
    const payload = parsePayloadJsonV2a(row.payload_json);
    const bucket = trendBucketKeyV2b(row.created_at, range);

    const overallAgg = getTrendAggV2b(overallMap, bucket);
    overallAgg.count += 1;
    addMetricV2b(overallAgg, "overall_score", recordOverallScoreV2b(row.type, payload));
    addMetricV2b(overallAgg, "avg_ms", payload.avg_ms);

    if (row.type === "star") {
      const agg = getTrendAggV2b(starMap, bucket);
      agg.count += 1;

      addMetricV2b(agg, "hits", payload.hits);
      addMetricV2b(agg, "hit_score", payload.hit_score);
      addMetricV2b(agg, "speed_score", payload.speed_score);
      addMetricV2b(agg, "avg_ms", payload.avg_ms);
      addMetricV2b(agg, "fastest_ms", payload.fastest_ms);
      addMetricV2b(agg, "difficulty", payload.difficulty);
      addMetricV2b(agg, "dwell_ms", payload.dwell_ms);
      addMetricV2b(agg, "target_r", payload.target_r);

      agg.best_hits = Math.max(agg.best_hits, Number(payload.hits || 0));
      agg.best_hit_score = Math.max(agg.best_hit_score, Number(payload.hit_score || 0));
    }

    if (row.type === "color_go") {
      const agg = getTrendAggV2b(colorMap, bucket);
      agg.count += 1;

      addMetricV2b(agg, "correct", payload.correct);
      addMetricV2b(agg, "wrong", payload.wrong);
      addMetricV2b(agg, "false_alarm", payload.false_alarm);
      addMetricV2b(agg, "miss", payload.miss);
      addMetricV2b(agg, "accuracy", payload.accuracy);
      addMetricV2b(agg, "inhibition", payload.inhibition);
      addMetricV2b(agg, "speed_score", payload.speed_score);
      addMetricV2b(agg, "avg_ms", payload.avg_ms);
      addMetricV2b(agg, "fastest_ms", payload.fastest_ms);
      addMetricV2b(agg, "difficulty", payload.difficulty);
      addMetricV2b(agg, "dwell_ms", payload.dwell_ms);
      addMetricV2b(agg, "bubble_count", payload.bubble_count);
      addMetricV2b(agg, "nogo_ratio", payload.nogo_ratio);

      agg.best_accuracy = Math.max(agg.best_accuracy, Number(payload.accuracy || 0));
      agg.best_inhibition = Math.max(agg.best_inhibition, Number(payload.inhibition || 0));
    }
  }

  return jsonResponse({
    ok: true,
    api: "trends_v2b",
    type,
    range,
    bucket: trendBucketModeV2b(range),
    from,
    to,
    limit,
    count: rows.length,
    series: {
      star: type === "color_go" ? [] : sortedTrendSeriesV2b(starMap, finalizeStarTrendV2b),
      color_go: type === "star" ? [] : sortedTrendSeriesV2b(colorMap, finalizeColorTrendV2b),
      overall: sortedTrendSeriesV2b(overallMap, finalizeOverallTrendV2b)
    }
  });
}


// -----------------------------------------------------------------------------
// Cloud-SD v2e config API
//
// Adds:
//   GET  /api/airtouch/config/latest?device_id=airtouch_001&user_id=child_001
//   POST /api/airtouch/config/update
//   POST /api/airtouch/config/ack
//
// Purpose:
//   Store cloud-side game parameters before ESP8266/P4 downlink is implemented.
// -----------------------------------------------------------------------------

function nowIsoV2e() {
  return new Date().toISOString();
}

function defaultDeviceIdV2e(value) {
  return String(value || "airtouch_001").trim() || "airtouch_001";
}

function defaultUserIdV2e(value) {
  return String(value || "child_001").trim() || "child_001";
}

function clampNumberV2e(value, fallback, min, max) {
  const n = Number(value);

  if (!Number.isFinite(n)) {
    return fallback;
  }

  return Math.max(min, Math.min(max, Math.round(n)));
}

function boolV2e(value, fallback) {
  if (value === true || value === 1 || value === "1" || value === "true") {
    return true;
  }

  if (value === false || value === 0 || value === "0" || value === "false") {
    return false;
  }

  return fallback;
}

function defaultConfigV2e() {
  return {
    config_file_version: 1,
    cloud_config_enabled: true,
    star: {
      target_radius: 56,
      dwell_ms: 336,
      round_duration_s: 45,
      difficulty: 2,
      adaptive_enabled: true
    },
    color_go: {
      target_radius: 54,
      dwell_ms: 520,
      bubble_count: 4,
      nogo_ratio: 25,
      round_duration_s: 45,
      difficulty: 2,
      adaptive_enabled: false
    },
    global: {
      distance_guard_enabled: true,
      difficulty_floor: 1,
      difficulty_ceiling: 5
    }
  };
}

function sanitizeConfigV2e(input) {
  const src = input || {};
  const base = defaultConfigV2e();

  const star = src.star || {};
  const color = src.color_go || src.color || {};
  const global = src.global || {};

  const difficultyFloor = clampNumberV2e(
    global.difficulty_floor,
    base.global.difficulty_floor,
    1,
    5
  );

  const difficultyCeiling = clampNumberV2e(
    global.difficulty_ceiling,
    base.global.difficulty_ceiling,
    difficultyFloor,
    5
  );

  return {
    config_file_version: 1,
    cloud_config_enabled: boolV2e(src.cloud_config_enabled, true),

    star: {
      target_radius: clampNumberV2e(star.target_radius, base.star.target_radius, 36, 90),
      dwell_ms: clampNumberV2e(star.dwell_ms, base.star.dwell_ms, 250, 1200),
      round_duration_s: clampNumberV2e(star.round_duration_s, base.star.round_duration_s, 30, 90),
      difficulty: clampNumberV2e(star.difficulty, base.star.difficulty, difficultyFloor, difficultyCeiling),
      adaptive_enabled: boolV2e(star.adaptive_enabled, base.star.adaptive_enabled)
    },

    color_go: {
      target_radius: clampNumberV2e(color.target_radius, base.color_go.target_radius, 36, 90),
      dwell_ms: clampNumberV2e(color.dwell_ms, base.color_go.dwell_ms, 300, 1200),
      bubble_count: clampNumberV2e(color.bubble_count, base.color_go.bubble_count, 3, 8),
      nogo_ratio: clampNumberV2e(color.nogo_ratio, base.color_go.nogo_ratio, 10, 50),
      round_duration_s: clampNumberV2e(color.round_duration_s, base.color_go.round_duration_s, 30, 90),
      difficulty: clampNumberV2e(color.difficulty, base.color_go.difficulty, difficultyFloor, difficultyCeiling),
      adaptive_enabled: false
    },

    global: {
      distance_guard_enabled: boolV2e(global.distance_guard_enabled, base.global.distance_guard_enabled),
      difficulty_floor: difficultyFloor,
      difficulty_ceiling: difficultyCeiling
    }
  };
}

function parseConfigJsonV2e(value) {
  if (!value) {
    return defaultConfigV2e();
  }

  try {
    return JSON.parse(value);
  } catch (err) {
    return defaultConfigV2e();
  }
}

async function latestConfigRowV2e(env, deviceId, userId) {
  return env.airtouch_db.prepare(
    `SELECT id, device_id, user_id, config_version, config_json, created_at, applied_version, applied_at
     FROM device_config
     WHERE device_id = ? AND user_id = ?
     ORDER BY config_version DESC, id DESC
     LIMIT 1`
  ).bind(deviceId, userId).first();
}

async function handleConfigLatestGet(request, env) {
  const url = new URL(request.url);

  const deviceId = defaultDeviceIdV2e(url.searchParams.get("device_id"));
  const userId = defaultUserIdV2e(url.searchParams.get("user_id"));

  const row = await latestConfigRowV2e(env, deviceId, userId);

  if (!row) {
    const config = defaultConfigV2e();
    config.config_version = 0;
    config.device_id = deviceId;
    config.user_id = userId;

    return jsonResponse({
      ok: true,
      api: "config_latest_v2e",
      has_config: false,
      device_id: deviceId,
      user_id: userId,
      config_version: 0,
      applied_version: 0,
      applied_at: null,
      created_at: null,
      config
    });
  }

  const config = parseConfigJsonV2e(row.config_json);
  config.config_version = row.config_version;
  config.device_id = row.device_id;
  config.user_id = row.user_id;

  return jsonResponse({
    ok: true,
    api: "config_latest_v2e",
    has_config: true,
    id: row.id,
    device_id: row.device_id,
    user_id: row.user_id,
    config_version: row.config_version,
    applied_version: row.applied_version || 0,
    applied_at: row.applied_at || null,
    created_at: row.created_at,
    config
  });
}

async function handleConfigUpdatePost(request, env) {
  const token = request.headers.get("X-AirTouch-Token") || "";
  if (token !== env.AIRTOUCH_TOKEN) {
    return jsonResponse({ ok: false, error: "UNAUTHORIZED" }, 401);
  }

  let body = {};
  try {
    body = await request.json();
  } catch (err) {
    return jsonResponse({ ok: false, error: "BAD_JSON" }, 400);
  }

  const deviceId = defaultDeviceIdV2e(body.device_id);
  const userId = defaultUserIdV2e(body.user_id);

  const latest = await latestConfigRowV2e(env, deviceId, userId);
  const nextVersion = latest ? Number(latest.config_version || 0) + 1 : 1;

  const config = sanitizeConfigV2e(body.config || body);
  config.config_version = nextVersion;
  config.device_id = deviceId;
  config.user_id = userId;
  config.updated_at = nowIsoV2e();

  const createdAt = nowIsoV2e();

  await env.airtouch_db.prepare(
    `INSERT INTO device_config
     (device_id, user_id, config_version, config_json, created_at, applied_version, applied_at)
     VALUES (?, ?, ?, ?, ?, ?, ?)`
  ).bind(
    deviceId,
    userId,
    nextVersion,
    JSON.stringify(config),
    createdAt,
    0,
    null
  ).run();

  return jsonResponse({
    ok: true,
    api: "config_update_v2e",
    device_id: deviceId,
    user_id: userId,
    config_version: nextVersion,
    created_at: createdAt,
    config
  });
}

async function handleConfigAckPost(request, env) {
  const token = request.headers.get("X-AirTouch-Token") || "";
  if (token !== env.AIRTOUCH_TOKEN) {
    return jsonResponse({ ok: false, error: "UNAUTHORIZED" }, 401);
  }

  let body = {};
  try {
    body = await request.json();
  } catch (err) {
    return jsonResponse({ ok: false, error: "BAD_JSON" }, 400);
  }

  const deviceId = defaultDeviceIdV2e(body.device_id);
  const userId = defaultUserIdV2e(body.user_id);
  const appliedVersion = Number(body.applied_version || body.config_version || 0);

  if (!Number.isFinite(appliedVersion) || appliedVersion <= 0) {
    return jsonResponse({ ok: false, error: "BAD_APPLIED_VERSION" }, 400);
  }

  const appliedAt = nowIsoV2e();

  await env.airtouch_db.prepare(
    `UPDATE device_config
     SET applied_version = ?, applied_at = ?
     WHERE device_id = ? AND user_id = ? AND config_version = ?`
  ).bind(
    appliedVersion,
    appliedAt,
    deviceId,
    userId,
    appliedVersion
  ).run();

  return jsonResponse({
    ok: true,
    api: "config_ack_v2e",
    device_id: deviceId,
    user_id: userId,
    applied_version: appliedVersion,
    applied_at: appliedAt
  });
}

function dashboardHtml() {
  return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover" />
  <title>AirTouch Cloud Dashboard</title>
  <style>
    :root {
      --bg0: #07111f;
      --bg1: #0b1730;
      --card: rgba(255,255,255,0.085);
      --card2: rgba(255,255,255,0.12);
      --line: rgba(255,255,255,0.16);
      --text: #eef6ff;
      --muted: rgba(238,246,255,0.68);
      --soft: rgba(238,246,255,0.42);
      --blue: #62a8ff;
      --cyan: #61f4ff;
      --green: #67f5a6;
      --yellow: #ffd166;
      --red: #ff6b7a;
      --purple: #b388ff;
      --shadow: 0 20px 70px rgba(0,0,0,0.38);
    }

    * {
      box-sizing: border-box;
    }

    html, body {
      margin: 0;
      min-height: 100%;
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", sans-serif;
      color: var(--text);
      background:
        radial-gradient(circle at 12% 8%, rgba(98,168,255,0.34), transparent 30%),
        radial-gradient(circle at 86% 12%, rgba(97,244,255,0.22), transparent 28%),
        radial-gradient(circle at 68% 82%, rgba(179,136,255,0.20), transparent 30%),
        linear-gradient(135deg, var(--bg0), var(--bg1) 55%, #0d1025);
      overflow-x: hidden;
    }

    body::before {
      content: "";
      position: fixed;
      inset: 0;
      pointer-events: none;
      background-image:
        linear-gradient(rgba(255,255,255,0.035) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255,255,255,0.035) 1px, transparent 1px);
      background-size: 46px 46px;
      mask-image: linear-gradient(to bottom, rgba(0,0,0,0.85), transparent 86%);
    }

    .wrap {
      width: min(1220px, calc(100vw - 34px));
      margin: 0 auto;
      padding: 28px 0 48px;
      position: relative;
      z-index: 1;
    }

    .hero {
      display: grid;
      grid-template-columns: 1.25fr 0.75fr;
      gap: 18px;
      align-items: stretch;
      margin-bottom: 18px;
    }

    .hero-main,
    .hero-side,
    .card {
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(255,255,255,0.13), rgba(255,255,255,0.065));
      backdrop-filter: blur(18px);
      border-radius: 28px;
      box-shadow: var(--shadow);
    }

    .hero-main {
      padding: 28px;
      min-height: 220px;
      position: relative;
      overflow: hidden;
    }

    .hero-main::after {
      content: "";
      position: absolute;
      width: 260px;
      height: 260px;
      border-radius: 999px;
      right: -90px;
      top: -95px;
      background: radial-gradient(circle, rgba(97,244,255,0.26), transparent 66%);
    }

    .eyebrow {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 8px 12px;
      border: 1px solid rgba(97,244,255,0.28);
      border-radius: 999px;
      color: rgba(238,246,255,0.82);
      background: rgba(97,244,255,0.08);
      font-size: 13px;
      letter-spacing: 0.03em;
    }

    .dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--green);
      box-shadow: 0 0 18px var(--green);
    }

    h1 {
      margin: 20px 0 12px;
      font-size: clamp(32px, 5vw, 58px);
      line-height: 1.02;
      letter-spacing: -0.055em;
    }

    .subtitle {
      max-width: 760px;
      color: var(--muted);
      line-height: 1.75;
      font-size: 16px;
    }

    .flow {
      margin-top: 22px;
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }

    .chip {
      border: 1px solid rgba(255,255,255,0.14);
      background: rgba(255,255,255,0.07);
      border-radius: 999px;
      padding: 8px 11px;
      color: rgba(238,246,255,0.80);
      font-size: 13px;
    }

    .hero-side {
      padding: 22px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      min-height: 220px;
    }

    .clock {
      font-size: 14px;
      color: var(--muted);
      line-height: 1.7;
    }

    .status-big {
      margin-top: 20px;
    }

    .status-label {
      color: var(--muted);
      font-size: 13px;
    }

    .status-value {
      font-size: 42px;
      line-height: 1;
      margin-top: 8px;
      font-weight: 800;
      letter-spacing: -0.05em;
    }

    .status-note {
      margin-top: 10px;
      color: var(--soft);
      font-size: 13px;
      line-height: 1.6;
    }

    .grid-kpi {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 14px;
      margin: 18px 0;
    }

    .kpi {
      padding: 20px;
      border-radius: 24px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.075);
      box-shadow: 0 16px 46px rgba(0,0,0,0.22);
      min-height: 132px;
      position: relative;
      overflow: hidden;
    }

    .kpi::after {
      content: "";
      position: absolute;
      width: 120px;
      height: 120px;
      border-radius: 50%;
      right: -50px;
      bottom: -60px;
      background: radial-gradient(circle, rgba(98,168,255,0.28), transparent 70%);
    }

    .kpi-title {
      color: var(--muted);
      font-size: 13px;
    }

    .kpi-value {
      margin-top: 14px;
      font-size: 38px;
      font-weight: 850;
      letter-spacing: -0.045em;
    }

    .kpi-unit {
      font-size: 15px;
      color: var(--muted);
      margin-left: 5px;
      font-weight: 500;
    }

    .kpi-desc {
      margin-top: 8px;
      color: var(--soft);
      font-size: 13px;
    }

    .section-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 14px;
      margin-bottom: 14px;
    }

    .card {
      padding: 20px;
      min-height: 280px;
    }

    .card-head {
      display: flex;
      justify-content: space-between;
      gap: 14px;
      align-items: flex-start;
      margin-bottom: 14px;
    }

    .card-title {
      font-size: 18px;
      font-weight: 750;
      letter-spacing: -0.02em;
    }

    .card-sub {
      margin-top: 5px;
      color: var(--muted);
      font-size: 13px;
      line-height: 1.5;
    }

    .pill {
      border: 1px solid rgba(255,255,255,0.14);
      background: rgba(255,255,255,0.08);
      color: rgba(238,246,255,0.80);
      border-radius: 999px;
      padding: 7px 10px;
      font-size: 12px;
      white-space: nowrap;
    }

    .metric-row {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 10px;
      margin-top: 14px;
    }

    .metric {
      background: rgba(255,255,255,0.07);
      border: 1px solid rgba(255,255,255,0.12);
      border-radius: 18px;
      padding: 13px;
      min-height: 82px;
    }

    .metric-name {
      color: var(--muted);
      font-size: 12px;
    }

    .metric-value {
      margin-top: 8px;
      font-size: 24px;
      font-weight: 800;
      letter-spacing: -0.035em;
    }

    .chart {
      width: 100%;
      height: 230px;
      display: block;
      margin-top: 10px;
      border-radius: 18px;
      background: rgba(0,0,0,0.12);
    }

    .wide {
      grid-column: 1 / -1;
    }

    .table-wrap {
      overflow-x: auto;
      border-radius: 20px;
      border: 1px solid rgba(255,255,255,0.10);
      margin-top: 12px;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      min-width: 780px;
      background: rgba(0,0,0,0.10);
    }

    th, td {
      padding: 13px 14px;
      text-align: left;
      border-bottom: 1px solid rgba(255,255,255,0.09);
      font-size: 13px;
    }

    th {
      color: rgba(238,246,255,0.65);
      font-weight: 650;
      background: rgba(255,255,255,0.045);
    }

    td {
      color: rgba(238,246,255,0.86);
    }

    tr:last-child td {
      border-bottom: none;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 5px 9px;
      border-radius: 999px;
      font-size: 12px;
      border: 1px solid rgba(255,255,255,0.14);
      background: rgba(255,255,255,0.07);
    }

    .badge.star {
      color: #ffe08a;
    }

    .badge.color_go {
      color: #8fffd2;
    }

    .footer {
      color: var(--soft);
      text-align: center;
      padding: 24px 0 0;
      font-size: 13px;
      line-height: 1.7;
    }

    .empty {
      color: var(--muted);
      padding: 30px 10px;
      text-align: center;
      border: 1px dashed rgba(255,255,255,0.16);
      border-radius: 18px;
      background: rgba(255,255,255,0.045);
    }

    .refresh-btn {
      cursor: pointer;
      border: 1px solid rgba(97,244,255,0.28);
      background: rgba(97,244,255,0.10);
      color: var(--text);
      border-radius: 999px;
      padding: 9px 13px;
      font-weight: 650;
    }

    .refresh-btn:active {
      transform: translateY(1px);
    }

    @media (max-width: 920px) {
      .hero,
      .section-grid {
        grid-template-columns: 1fr;
      }

      .grid-kpi {
        grid-template-columns: repeat(2, 1fr);
      }

      .metric-row {
        grid-template-columns: repeat(2, 1fr);
      }
    }

    @media (max-width: 560px) {
      .wrap {
        width: min(100vw - 22px, 1220px);
        padding-top: 14px;
      }

      .hero-main,
      .hero-side,
      .card {
        border-radius: 22px;
        padding: 18px;
      }

      .grid-kpi {
        grid-template-columns: 1fr;
      }

      .metric-row {
        grid-template-columns: 1fr 1fr;
      }

      .kpi {
        min-height: 112px;
      }

      .kpi-value {
        font-size: 34px;
      }

      .chart {
        height: 210px;
      }
    }
  </style>
</head>
<body>
  <main class="wrap">
    <section class="hero">
      <div class="hero-main">
        <div class="eyebrow"><span class="dot"></span>AirTouch Cloud Dashboard · Live</div>
        <h1>儿童空中无接触交互训练云端看板</h1>
        <div class="subtitle">
          基于 ESP32-P4 的端侧训练数据，经 SD 本地成长档案与 ESP8266 云网关同步至 Cloudflare Worker + D1，
          实现 Star Catcher 与 Color-Go 训练表现的远程查看、趋势分析与展示验证。
        </div>
        <div class="flow">
          <span class="chip">P4 训练采集</span>
          <span class="chip">SD 本地档案</span>
          <span class="chip">UART ATQ</span>
          <span class="chip">ESP8266 网关</span>
          <span class="chip">Worker API</span>
          <span class="chip">D1 云数据库</span>
        </div>
      </div>

      <div class="hero-side">
        <div>
          <div class="clock" id="clock">Loading...</div>
          <div class="status-big">
            <div class="status-label">云端连接状态</div>
            <div class="status-value" id="cloudStatus">--</div>
            <div class="status-note" id="statusNote">正在读取 D1 数据库。</div>
          </div>
        </div>
        <button class="refresh-btn" onclick="loadDashboard()">立即刷新</button>
      </div>
    </section>

    <section class="grid-kpi">
      <div class="kpi">
        <div class="kpi-title">总训练记录</div>
        <div class="kpi-value"><span id="totalRecords">--</span><span class="kpi-unit">次</span></div>
        <div class="kpi-desc">来自云端 D1 数据库</div>
      </div>
      <div class="kpi">
        <div class="kpi-title">Star Catcher</div>
        <div class="kpi-value"><span id="starCount">--</span><span class="kpi-unit">轮</span></div>
        <div class="kpi-desc">反应速度与手眼协调</div>
      </div>
      <div class="kpi">
        <div class="kpi-title">Color-Go</div>
        <div class="kpi-value"><span id="colorCount">--</span><span class="kpi-unit">轮</span></div>
        <div class="kpi-desc">选择反应与抑制控制</div>
      </div>
      <div class="kpi">
        <div class="kpi-title">最近同步</div>
        <div class="kpi-value" style="font-size:28px"><span id="lastSync">--</span></div>
        <div class="kpi-desc">Worker 接收时间</div>
      </div>
    </section>

    <section class="section-grid">
      <div class="card">
        <div class="card-head">
          <div>
            <div class="card-title">最新 Star Catcher 表现</div>
            <div class="card-sub">命中数量、反应速度与综合训练表现。</div>
          </div>
          <div class="pill" id="latestStarTime">--</div>
        </div>

        <div class="metric-row">
          <div class="metric">
            <div class="metric-name">命中数</div>
            <div class="metric-value" id="starHits">--</div>
          </div>
          <div class="metric">
            <div class="metric-name">平均反应</div>
            <div class="metric-value"><span id="starAvg">--</span><span class="kpi-unit">ms</span></div>
          </div>
          <div class="metric">
            <div class="metric-name">最快反应</div>
            <div class="metric-value"><span id="starFastest">--</span><span class="kpi-unit">ms</span></div>
          </div>
          <div class="metric">
            <div class="metric-name">命中评分</div>
            <div class="metric-value"><span id="starScore">--</span><span class="kpi-unit">分</span></div>
          </div>
        </div>

        <canvas id="starChart" class="chart"></canvas>
      </div>

      <div class="card">
        <div class="card-head">
          <div>
            <div class="card-title">最新 Color-Go 表现</div>
            <div class="card-sub">准确率、抑制控制与选择反应速度。</div>
          </div>
          <div class="pill" id="latestColorTime">--</div>
        </div>

        <div class="metric-row">
          <div class="metric">
            <div class="metric-name">准确率</div>
            <div class="metric-value"><span id="colorAccuracy">--</span><span class="kpi-unit">%</span></div>
          </div>
          <div class="metric">
            <div class="metric-name">抑制控制</div>
            <div class="metric-value"><span id="colorInhibit">--</span><span class="kpi-unit">分</span></div>
          </div>
          <div class="metric">
            <div class="metric-name">平均反应</div>
            <div class="metric-value"><span id="colorAvg">--</span><span class="kpi-unit">ms</span></div>
          </div>
          <div class="metric">
            <div class="metric-name">速度评分</div>
            <div class="metric-value"><span id="colorSpeed">--</span><span class="kpi-unit">分</span></div>
          </div>
        </div>

        <canvas id="colorChart" class="chart"></canvas>
      </div>

      <div class="card wide">
        <div class="card-head">
          <div>
            <div class="card-title">最近训练记录</div>
            <div class="card-sub">实时展示 P4 训练后同步到云端的 Star / Color-Go 记录。</div>
          </div>
          <div class="pill" id="recordCountPill">-- records</div>
        </div>

        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>ID</th>
                <th>类型</th>
                <th>轮次</th>
                <th>核心指标</th>
                <th>反应时间</th>
                <th>来源</th>
                <th>云端时间</th>
              </tr>
            </thead>
            <tbody id="recordTable">
              <tr><td colspan="7">Loading...</td></tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>

    <div class="footer">
      AirTouch 智训板 · 端侧采集 — 本地档案 — 云端同步 — 可视化评估<br/>
      Dashboard v1a · Cloudflare Worker + D1
    </div>
  </main>

<script>
const fmt = new Intl.DateTimeFormat("zh-CN", {
  month: "2-digit",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  second: "2-digit"
});

function $(id) {
  return document.getElementById(id);
}

function setText(id, value) {
  const el = $(id);
  if (el) el.textContent = value;
}

function formatTime(iso) {
  if (!iso) return "--";
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "--";
  return fmt.format(d);
}

function shortTime(iso) {
  if (!iso) return "--";
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "--";
  const now = Date.now();
  const diff = Math.max(0, now - d.getTime());
  const min = Math.floor(diff / 60000);
  if (min < 1) return "刚刚";
  if (min < 60) return min + " 分钟前";
  const h = Math.floor(min / 60);
  if (h < 24) return h + " 小时前";
  return fmt.format(d);
}

function drawLineChart(canvas, seriesList, labels, options = {}) {
  const ctx = canvas.getContext("2d");
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();

  canvas.width = Math.floor(rect.width * dpr);
  canvas.height = Math.floor(rect.height * dpr);

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  const w = rect.width;
  const h = rect.height;
  const pad = { left: 42, right: 18, top: 22, bottom: 34 };
  const innerW = w - pad.left - pad.right;
  const innerH = h - pad.top - pad.bottom;

  ctx.clearRect(0, 0, w, h);

  const allValues = seriesList.flatMap(s => s.data).filter(v => Number.isFinite(v));
  if (allValues.length === 0) {
    ctx.fillStyle = "rgba(238,246,255,0.58)";
    ctx.font = "14px system-ui";
    ctx.fillText("暂无趋势数据", pad.left, h / 2);
    return;
  }

  let min = options.min ?? Math.min(...allValues);
  let max = options.max ?? Math.max(...allValues);
  if (min === max) {
    min -= 1;
    max += 1;
  }

  const range = max - min;

  ctx.strokeStyle = "rgba(255,255,255,0.09)";
  ctx.lineWidth = 1;

  for (let i = 0; i <= 4; i++) {
    const y = pad.top + innerH * i / 4;
    ctx.beginPath();
    ctx.moveTo(pad.left, y);
    ctx.lineTo(w - pad.right, y);
    ctx.stroke();

    const val = Math.round(max - range * i / 4);
    ctx.fillStyle = "rgba(238,246,255,0.44)";
    ctx.font = "11px system-ui";
    ctx.fillText(String(val), 8, y + 4);
  }

  const n = Math.max(...seriesList.map(s => s.data.length));
  const xFor = (i) => n <= 1 ? pad.left + innerW / 2 : pad.left + innerW * i / (n - 1);
  const yFor = (v) => pad.top + innerH - ((v - min) / range) * innerH;

  seriesList.forEach((series) => {
    const grad = ctx.createLinearGradient(pad.left, 0, w - pad.right, 0);
    grad.addColorStop(0, series.colorA);
    grad.addColorStop(1, series.colorB);

    ctx.strokeStyle = grad;
    ctx.lineWidth = 3;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.beginPath();

    series.data.forEach((v, i) => {
      const x = xFor(i);
      const y = yFor(v);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });

    ctx.stroke();

    series.data.forEach((v, i) => {
      const x = xFor(i);
      const y = yFor(v);
      ctx.fillStyle = series.colorB;
      ctx.beginPath();
      ctx.arc(x, y, 4, 0, Math.PI * 2);
      ctx.fill();
    });
  });

  ctx.fillStyle = "rgba(238,246,255,0.48)";
  ctx.font = "11px system-ui";
  if (labels.length > 0) {
    ctx.fillText(labels[0], pad.left, h - 12);
    const last = labels[labels.length - 1];
    const tw = ctx.measureText(last).width;
    ctx.fillText(last, w - pad.right - tw, h - 12);
  }

  let lx = pad.left;
  seriesList.forEach((series) => {
    ctx.fillStyle = series.colorB;
    ctx.beginPath();
    ctx.arc(lx, 13, 4, 0, Math.PI * 2);
    ctx.fill();

    ctx.fillStyle = "rgba(238,246,255,0.70)";
    ctx.font = "12px system-ui";
    ctx.fillText(series.name, lx + 9, 17);
    lx += ctx.measureText(series.name).width + 40;
  });
}

function metricForRecord(r) {
  const p = r.payload || {};
  if (r.type === "star") {
    return "命中 " + (p.hits ?? "--") + " · 命中分 " + (p.hit_score ?? "--");
  }
  if (r.type === "color_go") {
    return "准确率 " + (p.accuracy ?? "--") + "% · 抑制 " + (p.inhibition ?? "--");
  }
  return "--";
}

function reactionForRecord(r) {
  const p = r.payload || {};
  const avg = p.avg_ms ?? "--";
  const fastest = p.fastest_ms ?? "--";
  return "avg " + avg + " ms · best " + fastest + " ms";
}

function renderTable(records) {
  const tbody = $("recordTable");

  if (!records.length) {
    tbody.innerHTML = '<tr><td colspan="7"><div class="empty">暂无云端记录。完成一轮训练后，数据会自动同步到这里。</div></td></tr>';
    return;
  }

  tbody.innerHTML = records.map((r) => {
    const badgeClass = r.type === "star" ? "star" : "color_go";
    const typeName = r.type === "star" ? "Star Catcher" : r.type === "color_go" ? "Color-Go" : r.type;
    return '<tr>' +
      '<td>#' + r.id + '</td>' +
      '<td><span class="badge ' + badgeClass + '">' + typeName + '</span></td>' +
      '<td>rid ' + r.rid + '</td>' +
      '<td>' + metricForRecord(r) + '</td>' +
      '<td>' + reactionForRecord(r) + '</td>' +
      '<td>' + (r.source || "--") + '</td>' +
      '<td>' + formatTime(r.created_at) + '</td>' +
    '</tr>';
  }).join("");
}


// -----------------------------------------------------------------------------
// Dashboard v2c range-aware charts
// -----------------------------------------------------------------------------

const DASHBOARD_RANGE_OPTIONS_V2C = [
  { value: "day", label: "日" },
  { value: "week", label: "周" },
  { value: "month", label: "月" },
  { value: "year", label: "年" }
];

let currentDashboardRangeV2C = "week";

function dashboardRangeLabelV2C(range) {
  const item = DASHBOARD_RANGE_OPTIONS_V2C.find(x => x.value === range);
  return item ? item.label : "周";
}

function formatTrendBucketLabelV2C(bucket, range) {
  const s = String(bucket || "");

  if (range === "day") {
    const t = s.split("T")[1] || "";
    return t.slice(0, 5) || s;
  }

  if (range === "year") {
    return s;
  }

  if (s.length >= 10) {
    return s.slice(5, 10);
  }

  return s;
}

function ensureRangeControlsV2C() {
  let host = document.getElementById("rangeControlsV2C");

  if (!host) {
    host = document.createElement("div");
    host.id = "rangeControlsV2C";

    const target = document.querySelector(".hero-main") || document.querySelector("main") || document.body;
    target.appendChild(host);
  }

  host.style.cssText = [
    "display:flex",
    "gap:8px",
    "align-items:center",
    "flex-wrap:wrap",
    "margin-top:16px"
  ].join(";");

  host.innerHTML = "";

  const title = document.createElement("span");
  title.textContent = "趋势范围";
  title.style.cssText = [
    "font-size:13px",
    "color:rgba(238,246,255,0.72)",
    "margin-right:2px"
  ].join(";");
  host.appendChild(title);

  for (const item of DASHBOARD_RANGE_OPTIONS_V2C) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.textContent = item.label;
    btn.dataset.range = item.value;

    const active = item.value === currentDashboardRangeV2C;
    btn.style.cssText = [
      "border:1px solid rgba(255,255,255,0.18)",
      "border-radius:999px",
      "padding:8px 14px",
      "font-weight:800",
      "cursor:pointer",
      "color:" + (active ? "#07111f" : "rgba(238,246,255,0.86)"),
      "background:" + (active ? "linear-gradient(135deg,#67f5a6,#61f4ff)" : "rgba(255,255,255,0.08)"),
      "box-shadow:" + (active ? "0 10px 24px rgba(97,244,255,0.22)" : "none")
    ].join(";");

    btn.addEventListener("click", () => {
      if (currentDashboardRangeV2C === item.value) {
        return;
      }

      currentDashboardRangeV2C = item.value;
      window.addEventListener("resize", function(){ scheduleChartRedraw("resize"); });
loadDashboard();
    });

    host.appendChild(btn);
  }
}

function safeNumberV2C(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : 0;
}

function latestPayloadByTypeV2C(records, type) {
  const rec = records.find(r => r.type === type);
  return rec ? { payload: rec.payload || {}, created_at: rec.created_at } : null;
}


// -----------------------------------------------------------------------------
// Dashboard v2c.1 local range controls
// -----------------------------------------------------------------------------

const DASHBOARD_RANGE_OPTIONS_V2C1 = [
  { value: "day", label: "日" },
  { value: "week", label: "周" },
  { value: "month", label: "月" },
  { value: "year", label: "年" }
];

let starRangeV2C1 = "week";
let colorRangeV2C1 = "week";
let tableRangeV2C1 = "week";

function dashboardRangeLabelV2C1(range) {
  const item = DASHBOARD_RANGE_OPTIONS_V2C1.find(x => x.value === range);
  return item ? item.label : "周";
}

function removeOldHeroRangeControlsV2C1() {
  const old = document.getElementById("rangeControlsV2C");
  if (old) {
    old.remove();
  }
}

function renderLocalRangeButtonsV2C1(host, titleText, activeRange, onSelect) {
  host.style.cssText = [
    "display:flex",
    "align-items:center",
    "gap:8px",
    "flex-wrap:wrap",
    "margin:12px 0 2px 0",
    "padding:0 4px"
  ].join(";");

  host.innerHTML = "";

  const title = document.createElement("span");
  title.textContent = titleText;
  title.style.cssText = [
    "font-size:12px",
    "font-weight:800",
    "color:rgba(238,246,255,0.62)",
    "margin-right:2px"
  ].join(";");
  host.appendChild(title);

  for (const item of DASHBOARD_RANGE_OPTIONS_V2C1) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.textContent = item.label;

    const active = item.value === activeRange;

    btn.style.cssText = [
      "border:1px solid rgba(255,255,255,0.16)",
      "border-radius:999px",
      "padding:6px 11px",
      "font-size:12px",
      "font-weight:900",
      "cursor:pointer",
      "color:" + (active ? "#07111f" : "rgba(238,246,255,0.82)"),
      "background:" + (active ? "linear-gradient(135deg,#67f5a6,#61f4ff)" : "rgba(255,255,255,0.07)"),
      "box-shadow:" + (active ? "0 8px 18px rgba(97,244,255,0.18)" : "none")
    ].join(";");

    btn.addEventListener("click", () => {
      if (item.value === activeRange) {
        return;
      }
      onSelect(item.value);
    });

    host.appendChild(btn);
  }
}

function ensureChartRangeControlV2C1(canvasId, hostId, titleText, activeRange, onSelect) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) {
    return;
  }

  let host = document.getElementById(hostId);
  if (!host) {
    host = document.createElement("div");
    host.id = hostId;
    canvas.insertAdjacentElement("afterend", host);
  }

  renderLocalRangeButtonsV2C1(host, titleText, activeRange, onSelect);
}

function ensureTableRangeControlV2C1() {
  const pill = document.getElementById("recordCountPill");
  if (!pill) {
    return;
  }

  let host = document.getElementById("tableRangeControlsV2C1");
  if (!host) {
    host = document.createElement("div");
    host.id = "tableRangeControlsV2C1";
    pill.insertAdjacentElement("afterend", host);
  }

  renderLocalRangeButtonsV2C1(host, "记录范围", tableRangeV2C1, (range) => {
    tableRangeV2C1 = range;
    loadDashboard();
  });
}

function ensureLocalRangeControlsV2C1() {
  removeOldHeroRangeControlsV2C1();

  ensureChartRangeControlV2C1("starChart", "starChartRangeControlsV2C1", "图表范围", starRangeV2C1, (range) => {
    starRangeV2C1 = range;
    loadDashboard();
  });

  ensureChartRangeControlV2C1("colorChart", "colorChartRangeControlsV2C1", "图表范围", colorRangeV2C1, (range) => {
    colorRangeV2C1 = range;
    loadDashboard();
  });

  ensureTableRangeControlV2C1();
}

function safeNumberV2C1(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : 0;
}

function sortRecordsAscV2C1(records) {
  return [...records].sort((a, b) => {
    const ta = Date.parse(a.created_at || "");
    const tb = Date.parse(b.created_at || "");
    if (Number.isFinite(ta) && Number.isFinite(tb) && ta !== tb) {
      return ta - tb;
    }
    return Number(a.rid || 0) - Number(b.rid || 0);
  });
}

function latestPayloadByTypeV2C1(records, type) {
  const rec = records.find(r => r.type === type);
  return rec ? { payload: rec.payload || {}, created_at: rec.created_at } : null;
}

function labelForRawRecordV2C1(record, range) {
  if (range === "day") {
    return shortTime(record.created_at || "");
  }

  return "rid " + record.rid;
}

function formatTrendBucketLabelV2C1(bucket, range) {
  const s = String(bucket || "");

  if (range === "day") {
    const t = s.split("T")[1] || "";
    return t.slice(0, 5) || s;
  }

  if (range === "year") {
    return s;
  }

  if (s.length >= 10) {
    return s.slice(5, 10);
  }

  return s;
}

function buildStarChartDataV2C1(starTrend, starRecords, range) {
  if (starTrend.length > 1) {
    return {
      labels: starTrend.map(x => formatTrendBucketLabelV2C1(x.bucket, range)),
      hits: starTrend.map(x => safeNumberV2C1(x.hits_avg)),
      score: starTrend.map(x => safeNumberV2C1(x.hit_score_avg))
    };
  }

  const rows = sortRecordsAscV2C1(starRecords);

  return {
    labels: rows.map(r => labelForRawRecordV2C1(r, range)),
    hits: rows.map(r => safeNumberV2C1(r.payload?.hits)),
    score: rows.map(r => safeNumberV2C1(r.payload?.hit_score))
  };
}

function buildColorChartDataV2C1(colorTrend, colorRecords, range) {
  if (colorTrend.length > 1) {
    return {
      labels: colorTrend.map(x => formatTrendBucketLabelV2C1(x.bucket, range)),
      accuracy: colorTrend.map(x => safeNumberV2C1(x.accuracy_avg)),
      inhibition: colorTrend.map(x => safeNumberV2C1(x.inhibition_avg))
    };
  }

  const rows = sortRecordsAscV2C1(colorRecords);

  return {
    labels: rows.map(r => labelForRawRecordV2C1(r, range)),
    accuracy: rows.map(r => safeNumberV2C1(r.payload?.accuracy)),
    inhibition: rows.map(r => safeNumberV2C1(r.payload?.inhibition))
  };
}



// -----------------------------------------------------------------------------
// Dashboard v2d.2 tab zones
// -----------------------------------------------------------------------------

const DASHBOARD_TABS_V2D2 = [
  { value: "overview", label: "概述" },
  { value: "star", label: "Star Catcher" },
  { value: "color", label: "Color-Go" },
  { value: "overall", label: "综合数据" }
];

let currentDashboardTabV2D2 = "overview";
let overallRangeV2D2 = "week";

function findIsolatedPanelV2D2(childId, otherIds) {
  const child = document.getElementById(childId);
  if (!child) {
    return null;
  }

  let current = child.parentElement;
  let best = current;

  while (current && current.parentElement && current.parentElement !== document.body) {
    const parent = current.parentElement;

    const parentHasOther = otherIds.some((id) => {
      const other = document.getElementById(id);
      return other && other !== child && parent.contains(other);
    });

    if (parentHasOther) {
      break;
    }

    best = parent;
    current = parent;
  }

  return best || child.parentElement;
}

function cardForCanvasV2D2(canvasId) {
  if (canvasId === "starChart") {
    return findIsolatedPanelV2D2("starChart", ["colorChart", "recordTable", "overallChartV2D2"]);
  }

  if (canvasId === "colorChart") {
    return findIsolatedPanelV2D2("colorChart", ["starChart", "recordTable", "overallChartV2D2"]);
  }

  return findIsolatedPanelV2D2(canvasId, ["starChart", "colorChart", "recordTable"]);
}

function tableCardV2D2() {
  return findIsolatedPanelV2D2("recordTable", ["starChart", "colorChart", "overallChartV2D2"]);
}
function setDisplayV2D2(el, visible) {
  if (!el) {
    return;
  }

  el.style.display = visible ? "" : "none";
}

function makeParamPlaceholderV2D2(id, title, text) {
  const box = document.createElement("div");
  box.id = id;
  box.style.cssText = [
    "margin-top:16px",
    "padding:18px",
    "border-radius:20px",
    "border:1px dashed rgba(103,245,166,0.34)",
    "background:rgba(103,245,166,0.07)"
  ].join(";");

  box.innerHTML = [
    '<div style="font-size:14px;font-weight:950;color:rgba(103,245,166,0.95);">',
      title,
    '</div>',
    '<div style="margin-top:6px;font-size:13px;line-height:1.6;color:rgba(238,246,255,0.68);">',
      text,
    '</div>'
  ].join("");

  return box;
}

function ensureParamPlaceholdersV2D2() {
  const starCard = cardForCanvasV2D2("starChart");
  const colorCard = cardForCanvasV2D2("colorChart");

  if (starCard && !document.getElementById("starParamPlaceholderV2D2")) {
    starCard.appendChild(makeParamPlaceholderV2D2(
      "starParamPlaceholderV2D2",
      "Star Catcher 参数设置",
      "预留云端参数下放入口：目标大小、悬停确认时间、训练时长、难度上限等。"
    ));
  }

  if (colorCard && !document.getElementById("colorParamPlaceholderV2D2")) {
    colorCard.appendChild(makeParamPlaceholderV2D2(
      "colorParamPlaceholderV2D2",
      "Color-Go 参数设置",
      "预留云端参数下放入口：气泡数量、No-Go 比例、悬停确认时间、难度等级等。"
    ));
  }
}

function ensureMainTabsV2D2() {
  removeOldHeroRangeControlsV2C1();

  let host = document.getElementById("mainTabsV2D2");
  if (!host) {
    host = document.createElement("div");
    host.id = "mainTabsV2D2";

    const main = document.querySelector("main") || document.body;
    const before = document.querySelector(".grid-kpi");

    if (before && before.parentElement) {
      before.parentElement.insertBefore(host, before);
    } else {
      main.appendChild(host);
    }
  }

  host.style.cssText = [
    "display:flex",
    "gap:10px",
    "align-items:center",
    "flex-wrap:wrap",
    "margin:22px 0 18px 0",
    "padding:10px",
    "border-radius:999px",
    "background:rgba(255,255,255,0.055)",
    "border:1px solid rgba(255,255,255,0.10)"
  ].join(";");

  host.innerHTML = "";

  for (const item of DASHBOARD_TABS_V2D2) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.textContent = item.label;

    const active = item.value === currentDashboardTabV2D2;

    btn.style.cssText = [
      "border:1px solid rgba(255,255,255,0.14)",
      "border-radius:999px",
      "padding:10px 18px",
      "font-size:14px",
      "font-weight:950",
      "cursor:pointer",
      "color:" + (active ? "#07111f" : "rgba(238,246,255,0.82)"),
      "background:" + (active ? "linear-gradient(135deg,#67f5a6,#61f4ff)" : "rgba(255,255,255,0.07)"),
      "box-shadow:" + (active ? "0 10px 24px rgba(97,244,255,0.20)" : "none")
    ].join(";");

    btn.addEventListener("click", () => {
      if (currentDashboardTabV2D2 === item.value) {
        return;
      }

      currentDashboardTabV2D2 = item.value;
      loadDashboard();
    });

    host.appendChild(btn);
  }
}

function ensureOverallZoneV2D2() {
  let zone = document.getElementById("overallZoneV2D2");
  if (zone) {
    return zone;
  }

  const tableCard = tableCardV2D2();
  const main = document.querySelector("main") || document.body;

  zone = document.createElement("section");
  zone.id = "overallZoneV2D2";
  zone.style.cssText = [
    "margin:20px 0 0 0",
    "padding:26px",
    "border-radius:28px",
    "border:1px solid rgba(255,255,255,0.12)",
    "background:linear-gradient(145deg,rgba(255,255,255,0.095),rgba(255,255,255,0.045))",
    "box-shadow:0 18px 50px rgba(0,0,0,0.22)"
  ].join(";");

  zone.innerHTML = [
    '<div style="display:flex;justify-content:space-between;gap:16px;align-items:flex-start;flex-wrap:wrap;">',
      '<div>',
        '<div style="font-size:13px;font-weight:900;color:rgba(103,245,166,0.95);letter-spacing:.08em;text-transform:uppercase;">Overall</div>',
        '<h2 style="margin:6px 0 4px 0;font-size:26px;color:#eef6ff;">综合数据</h2>',
        '<div style="font-size:14px;color:rgba(238,246,255,0.68);">融合 Star Catcher 与 Color-Go，展示总体训练活跃度、综合表现和长期成长趋势。</div>',
      '</div>',
      '<div id="overallRangePillV2D2" style="padding:10px 14px;border-radius:999px;background:rgba(255,255,255,0.08);color:rgba(238,246,255,0.78);font-weight:800;">周 · 综合</div>',
    '</div>',

    '<div style="display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px;margin-top:20px;">',
      '<div style="padding:16px;border-radius:18px;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.10);">',
        '<div style="font-size:13px;color:rgba(238,246,255,0.62);">训练次数</div>',
        '<div style="margin-top:8px;font-size:32px;font-weight:950;color:#eef6ff;"><span id="overallTotalV2D2">--</span><span style="font-size:15px;margin-left:4px;">次</span></div>',
      '</div>',
      '<div style="padding:16px;border-radius:18px;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.10);">',
        '<div style="font-size:13px;color:rgba(238,246,255,0.62);">综合评分</div>',
        '<div style="margin-top:8px;font-size:32px;font-weight:950;color:#eef6ff;"><span id="overallScoreV2D2">--</span><span style="font-size:15px;margin-left:4px;">分</span></div>',
      '</div>',
      '<div style="padding:16px;border-radius:18px;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.10);">',
        '<div style="font-size:13px;color:rgba(238,246,255,0.62);">平均反应</div>',
        '<div style="margin-top:8px;font-size:32px;font-weight:950;color:#eef6ff;"><span id="overallAvgMsV2D2">--</span><span style="font-size:15px;margin-left:4px;">ms</span></div>',
      '</div>',
      '<div style="padding:16px;border-radius:18px;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.10);">',
        '<div style="font-size:13px;color:rgba(238,246,255,0.62);">最好表现</div>',
        '<div style="margin-top:8px;font-size:32px;font-weight:950;color:#eef6ff;"><span id="overallBestV2D2">--</span><span style="font-size:15px;margin-left:4px;">分</span></div>',
      '</div>',
    '</div>',

    '<div style="margin-top:18px;padding:18px;border-radius:22px;background:rgba(7,17,31,0.42);">',
      '<canvas id="overallChartV2D2" style="width:100%;height:280px;"></canvas>',
    '</div>',

    '<div id="overallRangeControlsV2D2"></div>'
  ].join("");

  if (tableCard && tableCard.parentElement) {
    tableCard.insertAdjacentElement("afterend", zone);
  } else {
    main.appendChild(zone);
  }

  return zone;
}

function ensureOverallRangeControlsV2D2() {
  const host = document.getElementById("overallRangeControlsV2D2");
  if (!host) {
    return;
  }

  renderLocalRangeButtonsV2C1(host, "综合范围", overallRangeV2D2, (range) => {
    overallRangeV2D2 = range;
    loadDashboard();
  });
}

function applyDashboardTabV2D2() {
  const kpi = document.querySelector(".grid-kpi");
  const starPanel = getStarTabPanelV2F3();
  const colorPanel = getColorTabPanelV2F3();
  const tableCard = tableCardV2D2();
  const overallZone = ensureOverallZoneV2D2();

  const tab = currentDashboardTabV2D2;

  setDisplayV2D2(kpi, tab === "overview");
  setDisplayV2D2(tableCard, tab === "overview");
  setDisplayV2D2(starPanel, tab === "star");
  setDisplayV2D2(colorPanel, tab === "color");
  setDisplayV2D2(overallZone, tab === "overall");
}
function averageNumbersV2D2(values) {
  const nums = values
    .map(v => Number(v))
    .filter(v => Number.isFinite(v) && v > 0);

  if (nums.length === 0) {
    return 0;
  }

  return nums.reduce((a, b) => a + b, 0) / nums.length;
}

function recordOverallScoreV2D2(record) {
  const p = record.payload || {};

  if (record.type === "star") {
    return averageNumbersV2D2([p.hit_score, p.speed_score]);
  }

  if (record.type === "color_go") {
    return averageNumbersV2D2([p.accuracy, p.inhibition, p.speed_score]);
  }

  return 0;
}

function buildOverallChartDataV2D2(overallTrend, overallRecords, range) {
  if (overallTrend.length > 1) {
    return {
      labels: overallTrend.map(x => formatTrendBucketLabelV2C1(x.bucket, range)),
      score: overallTrend.map(x => safeNumberV2C1(x.overall_score_avg)),
      avgMs: overallTrend.map(x => safeNumberV2C1(x.avg_ms_avg))
    };
  }

  const rows = sortRecordsAscV2C1(overallRecords);

  return {
    labels: rows.map(r => labelForRawRecordV2C1(r, range)),
    score: rows.map(r => Math.round(recordOverallScoreV2D2(r))),
    avgMs: rows.map(r => safeNumberV2C1(r.payload?.avg_ms))
  };
}

function updateOverallCardsV2D2(records) {
  const scores = records.map(recordOverallScoreV2D2).filter(v => v > 0);
  const avgScore = averageNumbersV2D2(scores);
  const bestScore = scores.length ? Math.max(...scores) : 0;
  const avgMs = averageNumbersV2D2(records.map(r => r.payload?.avg_ms));

  setText("overallTotalV2D2", records.length);
  setText("overallScoreV2D2", avgScore ? Math.round(avgScore) : "--");
  setText("overallAvgMsV2D2", avgMs ? Math.round(avgMs) : "--");
  setText("overallBestV2D2", bestScore ? Math.round(bestScore) : "--");
  setText("overallRangePillV2D2", dashboardRangeLabelV2C1(overallRangeV2D2) + " · 综合");
}


// -----------------------------------------------------------------------------
// Dashboard v2f config panels
// -----------------------------------------------------------------------------

let currentConfigV2F = null;
let configLoadedV2F = false;

function getV2F(id) {
  return document.getElementById(id);
}

function readNumberV2F(id, fallback) {
  const el = getV2F(id);
  if (!el) {
    return fallback;
  }

  const n = Number(el.value);
  return Number.isFinite(n) ? n : fallback;
}

function readCheckedV2F(id, fallback) {
  const el = getV2F(id);
  if (!el) {
    return fallback;
  }

  return !!el.checked;
}

function setInputValueV2F(id, value) {
  const el = getV2F(id);
  if (el) {
    el.value = value;
  }

  const label = getV2F(id + "Value");
  if (label) {
    label.textContent = String(value);
  }
}

function setInputCheckedV2F(id, value) {
  const el = getV2F(id);
  if (el) {
    el.checked = !!value;
  }
}

function tokenV2F(){ return "airtouch_demo_token_2026"; }

function requireTokenV2F() {
  let token = tokenV2F();

  if (!token) {
    token = "airtouch_demo_token_2026";
    token = token.trim();

    if (token) {
      localStorage.setItem("airtouch_config_token_v2f", token);
    }
  }

  return token;
}

function clearTokenV2F() {
  localStorage.removeItem("airtouch_config_token_v2f");
  setConfigStatusV2F("已清除本地保存的 Token。");
}

function defaultConfigClientV2F() {
  return {
    star: {
      target_radius: 56,
      dwell_ms: 336,
      round_duration_s: 45,
      difficulty: 2,
      adaptive_enabled: true
    },
    color_go: {
      target_radius: 54,
      dwell_ms: 520,
      bubble_count: 4,
      nogo_ratio: 25,
      round_duration_s: 45,
      difficulty: 2,
      adaptive_enabled: false
    },
    global: {
      distance_guard_enabled: true,
      difficulty_floor: 1,
      difficulty_ceiling: 5
    }
  };
}

function setConfigStatusV2F(text) {
  const ids = ["starConfigStatusV2F", "colorConfigStatusV2F"];

  ids.forEach((id) => {
    const el = getV2F(id);
    if (el) {
      el.textContent = text;
    }
  });
}

function configPanelShellV2F(id, title, subtitle, bodyHtml) {
  return [
    '<div id="' + id + '" style="margin-top:16px;padding:18px;border-radius:22px;border:1px solid rgba(103,245,166,0.24);background:rgba(103,245,166,0.06);">',
      '<div style="display:flex;justify-content:space-between;gap:12px;align-items:flex-start;flex-wrap:wrap;">',
        '<div>',
          '<div style="font-size:15px;font-weight:950;color:rgba(103,245,166,0.96);">' + title + '</div>',
          '<div style="margin-top:5px;font-size:13px;line-height:1.6;color:rgba(238,246,255,0.68);">' + subtitle + '</div>',
        '</div>',
        '<button type="button" data-action="clear-token-v2f" style="border:1px solid rgba(255,255,255,0.14);border-radius:999px;padding:7px 12px;background:rgba(255,255,255,0.06);color:rgba(238,246,255,0.72);font-weight:800;cursor:pointer;">清除 Token</button>',
      '</div>',
      bodyHtml,
    '</div>'
  ].join("");
}

function rangeRowV2F(label, id, min, max, step, unit) {
  return [
    '<div style="margin-top:14px;">',
      '<div style="display:flex;justify-content:space-between;gap:12px;font-size:13px;color:rgba(238,246,255,0.72);">',
        '<span>' + label + '</span>',
        '<b><span id="' + id + 'Value">--</span>' + unit + '</b>',
      '</div>',
      '<input id="' + id + '" type="range" min="' + min + '" max="' + max + '" step="' + step + '" style="width:100%;margin-top:8px;">',
    '</div>'
  ].join("");
}

function checkRowV2F(label, id) {
  return [
    '<label style="margin-top:14px;display:flex;align-items:center;gap:10px;font-size:13px;color:rgba(238,246,255,0.75);">',
      '<input id="' + id + '" type="checkbox" style="width:18px;height:18px;">',
      '<span>' + label + '</span>',
    '</label>'
  ].join("");
}

function starConfigHtmlV2F() {
  return configPanelShellV2F(
    "starConfigPanelV2F",
    "Star Catcher 参数设置",
    "保存后进入云端参数表，后续由 ESP8266 / P4 拉取并写入 SD CONFIG.TXT。",
    [
      '<div style="display:grid;grid-template-columns:minmax(0,1fr) 260px;gap:18px;margin-top:16px;align-items:start;">',
        '<div>',
          rangeRowV2F("目标大小", "starTargetRadiusV2F", 36, 90, 1, " px"),
          rangeRowV2F("悬停确认时间", "starDwellMsV2F", 250, 1200, 10, " ms"),
          rangeRowV2F("训练时长", "starDurationV2F", 30, 90, 5, " s"),
          rangeRowV2F("训练强度", "starDifficultyV2F", 1, 5, 1, ""),
          checkRowV2F("启用自适应难度", "starAdaptiveV2F"),
        '</div>',
        '<div style="padding:14px;border-radius:18px;background:rgba(7,17,31,0.38);border:1px solid rgba(255,255,255,0.10);">',
          '<div style="font-size:13px;font-weight:900;color:rgba(238,246,255,0.78);">效果预览</div>',
          '<canvas id="starPreviewV2F" width="240" height="170" style="width:100%;height:170px;margin-top:8px;border-radius:16px;background:rgba(0,0,0,0.16);"></canvas>',
          '<div style="margin-top:8px;font-size:12px;color:rgba(238,246,255,0.55);">圆越大越容易命中；悬停时间越长，确认越稳但反应更慢。</div>',
        '</div>',
      '</div>',
      '<div style="display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:16px;">',
        '<button type="button" id="saveStarConfigV2F" style="border:0;border-radius:999px;padding:10px 18px;background:linear-gradient(135deg,#67f5a6,#61f4ff);color:#07111f;font-weight:950;cursor:pointer;">保存 Star 参数到云端</button>',
        '<button type="button" id="reloadConfigV2F_A" style="border:1px solid rgba(255,255,255,0.16);border-radius:999px;padding:10px 16px;background:rgba(255,255,255,0.07);color:rgba(238,246,255,0.82);font-weight:900;cursor:pointer;">重新读取</button>',
        '<span id="starConfigStatusV2F" style="font-size:13px;color:rgba(238,246,255,0.62);">参数状态：--</span>',
      '</div>'
    ].join("")
  );
}

function colorConfigHtmlV2F() {
  return configPanelShellV2F(
    "colorConfigPanelV2F",
    "Color-Go 参数设置",
    "保存后进入云端参数表，后续由 ESP8266 / P4 拉取并写入 SD CONFIG.TXT。",
    [
      '<div style="display:grid;grid-template-columns:minmax(0,1fr) 260px;gap:18px;margin-top:16px;align-items:start;">',
        '<div>',
          rangeRowV2F("目标大小", "colorTargetRadiusV2F", 36, 90, 1, " px"),
          rangeRowV2F("悬停确认时间", "colorDwellMsV2F", 300, 1200, 10, " ms"),
          rangeRowV2F("气泡数量", "colorBubbleCountV2F", 3, 8, 1, " 个"),
          rangeRowV2F("No-Go 比例", "colorNogoRatioV2F", 10, 50, 5, " %"),
          rangeRowV2F("训练时长", "colorDurationV2F", 30, 90, 5, " s"),
          rangeRowV2F("训练强度", "colorDifficultyV2F", 1, 5, 1, ""),
        '</div>',
        '<div style="padding:14px;border-radius:18px;background:rgba(7,17,31,0.38);border:1px solid rgba(255,255,255,0.10);">',
          '<div style="font-size:13px;font-weight:900;color:rgba(238,246,255,0.78);">效果预览</div>',
          '<canvas id="colorPreviewV2F" width="240" height="170" style="width:100%;height:170px;margin-top:8px;border-radius:16px;background:rgba(0,0,0,0.16);"></canvas>',
          '<div style="margin-top:8px;font-size:12px;color:rgba(238,246,255,0.55);">紫色为 No-Go，绿色为 Go；No-Go 比例越高，抑制控制训练越强。</div>',
        '</div>',
      '</div>',
      '<div style="display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:16px;">',
        '<button type="button" id="saveColorConfigV2F" style="border:0;border-radius:999px;padding:10px 18px;background:linear-gradient(135deg,#67f5a6,#61f4ff);color:#07111f;font-weight:950;cursor:pointer;">保存 Color-Go 参数到云端</button>',
        '<button type="button" id="reloadConfigV2F_B" style="border:1px solid rgba(255,255,255,0.16);border-radius:999px;padding:10px 16px;background:rgba(255,255,255,0.07);color:rgba(238,246,255,0.82);font-weight:900;cursor:pointer;">重新读取</button>',
        '<span id="colorConfigStatusV2F" style="font-size:13px;color:rgba(238,246,255,0.62);">参数状态：--</span>',
      '</div>'
    ].join("")
  );
}

function ensureParamPlaceholdersV2D2() {
  const starCard = cardForCanvasV2D2("starChart");
  const colorCard = cardForCanvasV2D2("colorChart");

  if (starCard && !document.getElementById("starConfigPanelV2F")) {
    const old = document.getElementById("starParamPlaceholderV2D2");
    if (old) {
      old.remove();
    }

    const wrap = document.createElement("div");
    wrap.innerHTML = starConfigHtmlV2F();
    starCard.appendChild(wrap.firstChild);
  }

  if (colorCard && !document.getElementById("colorConfigPanelV2F")) {
    const old = document.getElementById("colorParamPlaceholderV2D2");
    if (old) {
      old.remove();
    }

    const wrap = document.createElement("div");
    wrap.innerHTML = colorConfigHtmlV2F();
    colorCard.appendChild(wrap.firstChild);
  }

  bindConfigEventsV2F();

  if (!configLoadedV2F) {
    refreshConfigV2F(false);
  }
}

function bindOneRangeV2F(id) {
  const el = getV2F(id);
  if (!el || el.dataset.boundV2F === "1") {
    return;
  }

  el.dataset.boundV2F = "1";

  el.addEventListener("input", () => {
    const label = getV2F(id + "Value");
    if (label) {
      label.textContent = el.value;
    }
    drawConfigPreviewsV2F();
  });
}

function bindConfigEventsV2F() {
  [
    "starTargetRadiusV2F",
    "starDwellMsV2F",
    "starDurationV2F",
    "starDifficultyV2F",
    "colorTargetRadiusV2F",
    "colorDwellMsV2F",
    "colorBubbleCountV2F",
    "colorNogoRatioV2F",
    "colorDurationV2F",
    "colorDifficultyV2F"
  ].forEach(bindOneRangeV2F);

  ["starAdaptiveV2F"].forEach((id) => {
    const el = getV2F(id);
    if (el && el.dataset.boundV2F !== "1") {
      el.dataset.boundV2F = "1";
      el.addEventListener("change", drawConfigPreviewsV2F);
    }
  });

  const saveStar = getV2F("saveStarConfigV2F");
  if (saveStar && saveStar.dataset.boundV2F !== "1") {
    saveStar.dataset.boundV2F = "1";
    saveStar.addEventListener("click", () => saveConfigV2F("star"));
  }

  const saveColor = getV2F("saveColorConfigV2F");
  if (saveColor && saveColor.dataset.boundV2F !== "1") {
    saveColor.dataset.boundV2F = "1";
    saveColor.addEventListener("click", () => saveConfigV2F("color_go"));
  }

  ["reloadConfigV2F_A", "reloadConfigV2F_B"].forEach((id) => {
    const el = getV2F(id);
    if (el && el.dataset.boundV2F !== "1") {
      el.dataset.boundV2F = "1";
      el.addEventListener("click", () => refreshConfigV2F(true));
    }
  });

  document.querySelectorAll("[data-action='clear-token-v2f']").forEach((el) => {
    if (el.dataset.boundV2F !== "1") {
      el.dataset.boundV2F = "1";
      el.addEventListener("click", clearTokenV2F);
    }
  });
}

async function refreshConfigV2F(force) {
  if (configLoadedV2F && !force) {
    return;
  }

  try {
    setConfigStatusV2F("正在读取云端参数...");

    const res = await fetch("/api/airtouch/config/latest?device_id=airtouch_001&user_id=child_001", {
      cache: "no-store"
    });

    if (!res.ok) {
      throw new Error("config latest failed");
    }

    const body = await res.json();
    const cfg = body.config || defaultConfigClientV2F();

    currentConfigV2F = cfg;
    configLoadedV2F = true;

    populateConfigFormV2F(cfg);

    const state = body.has_config
      ? "云端版本 v" + body.config_version + "，设备已应用 v" + (body.applied_version || 0)
      : "当前使用默认参数，尚未保存云端版本";

    setConfigStatusV2F(state);
  } catch (err) {
    console.error(err);
    currentConfigV2F = defaultConfigClientV2F();
    populateConfigFormV2F(currentConfigV2F);
    setConfigStatusV2F("读取云端参数失败，已显示默认参数。");
  }
}

function populateConfigFormV2F(cfg) {
  const star = cfg.star || defaultConfigClientV2F().star;
  const color = cfg.color_go || defaultConfigClientV2F().color_go;

  setInputValueV2F("starTargetRadiusV2F", star.target_radius);
  setInputValueV2F("starDwellMsV2F", star.dwell_ms);
  setInputValueV2F("starDurationV2F", star.round_duration_s);
  setInputValueV2F("starDifficultyV2F", star.difficulty);
  setInputCheckedV2F("starAdaptiveV2F", star.adaptive_enabled);

  setInputValueV2F("colorTargetRadiusV2F", color.target_radius);
  setInputValueV2F("colorDwellMsV2F", color.dwell_ms);
  setInputValueV2F("colorBubbleCountV2F", color.bubble_count);
  setInputValueV2F("colorNogoRatioV2F", color.nogo_ratio);
  setInputValueV2F("colorDurationV2F", color.round_duration_s);
  setInputValueV2F("colorDifficultyV2F", color.difficulty);

  drawConfigPreviewsV2F();
}

function collectConfigV2F() {
  const base = currentConfigV2F || defaultConfigClientV2F();

  return {
    cloud_config_enabled: true,

    star: {
      target_radius: readNumberV2F("starTargetRadiusV2F", base.star.target_radius),
      dwell_ms: readNumberV2F("starDwellMsV2F", base.star.dwell_ms),
      round_duration_s: readNumberV2F("starDurationV2F", base.star.round_duration_s),
      difficulty: readNumberV2F("starDifficultyV2F", base.star.difficulty),
      adaptive_enabled: readCheckedV2F("starAdaptiveV2F", base.star.adaptive_enabled)
    },

    color_go: {
      target_radius: readNumberV2F("colorTargetRadiusV2F", base.color_go.target_radius),
      dwell_ms: readNumberV2F("colorDwellMsV2F", base.color_go.dwell_ms),
      bubble_count: readNumberV2F("colorBubbleCountV2F", base.color_go.bubble_count),
      nogo_ratio: readNumberV2F("colorNogoRatioV2F", base.color_go.nogo_ratio),
      round_duration_s: readNumberV2F("colorDurationV2F", base.color_go.round_duration_s),
      difficulty: readNumberV2F("colorDifficultyV2F", base.color_go.difficulty),
      adaptive_enabled: false
    },

    global: {
      distance_guard_enabled: true,
      difficulty_floor: 1,
      difficulty_ceiling: 5
    }
  };
}

async function saveConfigV2F(scope) {
  const token = requireTokenV2F();

  if (!token) {
    setConfigStatusV2F("未输入 Token，保存已取消。");
    return;
  }

  try {
    setConfigStatusV2F("正在保存参数到云端...");

    const body = {
      device_id: "airtouch_001",
      user_id: "child_001",
      config: collectConfigV2F()
    };

    const res = await fetch("/api/airtouch/config/update", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "X-AirTouch-Token": "airtouch_demo_token_2026"
      },
      body: JSON.stringify(body)
    });

    const data = await res.json();

    if (!res.ok || !data.ok) {
      throw new Error(data.error || "save failed");
    }

    currentConfigV2F = data.config;
    configLoadedV2F = true;

    populateConfigFormV2F(currentConfigV2F);
    setConfigStatusV2F("保存成功：云端版本 v" + data.config_version + "，等待设备同步。");
  } catch (err) {
    console.error(err);
    setConfigStatusV2F("保存失败：" + err.message);
  }
}

function drawStarPreviewV2F() {
  const canvas = getV2F("starPreviewV2F");
  if (!canvas) {
    return;
  }

  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;

  const r = readNumberV2F("starTargetRadiusV2F", 56);
  const dwell = readNumberV2F("starDwellMsV2F", 336);
  const difficulty = readNumberV2F("starDifficultyV2F", 2);

  ctx.clearRect(0, 0, w, h);

  ctx.fillStyle = "rgba(255,255,255,0.05)";
  ctx.fillRect(0, 0, w, h);

  ctx.strokeStyle = "rgba(255,255,255,0.10)";
  ctx.lineWidth = 1;
  for (let i = 1; i <= 3; i++) {
    ctx.beginPath();
    ctx.arc(w / 2, h / 2, 24 * i, 0, Math.PI * 2);
    ctx.stroke();
  }

  ctx.fillStyle = "rgba(255,209,102,0.92)";
  ctx.beginPath();
  ctx.arc(w / 2, h / 2, Math.max(16, Math.min(62, r / 1.25)), 0, Math.PI * 2);
  ctx.fill();

  ctx.fillStyle = "rgba(97,244,255,0.95)";
  ctx.beginPath();
  ctx.arc(w / 2 + 48, h / 2 - 38, 7 + difficulty * 2, 0, Math.PI * 2);
  ctx.fill();

  ctx.fillStyle = "rgba(238,246,255,0.72)";
  ctx.font = "12px system-ui";
  ctx.fillText("target " + r + "px", 14, 24);
  ctx.fillText("dwell " + dwell + "ms", 14, 42);
  ctx.fillText("intensity " + difficulty, 14, 60);
}

function drawColorPreviewV2F() {
  const canvas = getV2F("colorPreviewV2F");
  if (!canvas) {
    return;
  }

  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;

  const count = readNumberV2F("colorBubbleCountV2F", 4);
  const nogo = readNumberV2F("colorNogoRatioV2F", 25);
  const r = readNumberV2F("colorTargetRadiusV2F", 54);
  const dwell = readNumberV2F("colorDwellMsV2F", 520);

  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "rgba(255,255,255,0.05)";
  ctx.fillRect(0, 0, w, h);

  const positions = [
    [54, 54], [122, 45], [186, 62], [75, 122],
    [150, 122], [205, 118], [42, 98], [118, 88]
  ];

  const nogoCount = Math.max(1, Math.round(count * nogo / 100));

  for (let i = 0; i < count; i++) {
    const p = positions[i % positions.length];
    const isNogo = i < nogoCount;

    ctx.fillStyle = isNogo ? "rgba(179,136,255,0.92)" : "rgba(103,245,166,0.92)";
    ctx.beginPath();
    ctx.arc(p[0], p[1], Math.max(12, Math.min(34, r / 2.1)), 0, Math.PI * 2);
    ctx.fill();

    ctx.fillStyle = "rgba(7,17,31,0.82)";
    ctx.font = "bold 12px system-ui";
    ctx.fillText(isNogo ? "N" : "G", p[0] - 4, p[1] + 4);
  }

  ctx.fillStyle = "rgba(238,246,255,0.72)";
  ctx.font = "12px system-ui";
  ctx.fillText(count + " bubbles · No-Go " + nogo + "%", 14, 24);
  ctx.fillText("dwell " + dwell + "ms", 14, 42);
}

function drawConfigPreviewsV2F() {
  drawStarPreviewV2F();
  drawColorPreviewV2F();
}


// -----------------------------------------------------------------------------
// Dashboard v2f.1 side config layout
// -----------------------------------------------------------------------------

function moveConfigPanelToSideV2F1(cardId, panelId, layoutId) {
  const panel = document.getElementById(panelId);
  if (!panel) {
    return;
  }

  const card = cardForCanvasV2D2(cardId);
  if (!card) {
    return;
  }

  let layout = document.getElementById(layoutId);

  if (!layout) {
    layout = document.createElement("div");
    layout.id = layoutId;

    layout.style.cssText = [
      "display:grid",
      "grid-template-columns:minmax(0,1fr) minmax(360px,430px)",
      "gap:18px",
      "align-items:start",
      "width:100%",
      "margin-top:0"
    ].join(";");

    card.insertAdjacentElement("beforebegin", layout);
    layout.appendChild(card);
  }

  if (panel.parentElement !== layout) {
    layout.appendChild(panel);
  }

  card.style.width = "100%";
  card.style.margin = "0";
  panel.style.marginTop = "0";
  panel.style.height = "fit-content";
}

function ensureSideConfigLayoutV2F1() {
  moveConfigPanelToSideV2F1("starChart", "starConfigPanelV2F", "starTabLayoutV2F1");
  moveConfigPanelToSideV2F1("colorChart", "colorConfigPanelV2F", "colorTabLayoutV2F1");
}

function getStarTabPanelV2F1() {
  return document.getElementById("starTabLayoutV2F1") || cardForCanvasV2D2("starChart");
}

function getColorTabPanelV2F1() {
  return document.getElementById("colorTabLayoutV2F1") || cardForCanvasV2D2("colorChart");
}


// -----------------------------------------------------------------------------
// Dashboard v2f.2 force side layout
// -----------------------------------------------------------------------------

function applySideLayoutStyleV2F2(layout) {
  if (!layout) {
    return;
  }

  layout.style.cssText = [
    "display:grid",
    "grid-template-columns:minmax(0,1.35fr) minmax(380px,0.65fr)",
    "gap:18px",
    "align-items:start",
    "width:100%",
    "margin:0"
  ].join(";");
}

function forcePanelBesideChartV2F2(panelId, layoutId) {
  const panel = document.getElementById(panelId);
  if (!panel) {
    return null;
  }

  let layout = document.getElementById(layoutId);

  if (layout) {
    applySideLayoutStyleV2F2(layout);

    if (panel.parentElement !== layout) {
      layout.appendChild(panel);
    }

    panel.style.marginTop = "0";
    panel.style.height = "fit-content";
    return layout;
  }

  const card = panel.parentElement;
  if (!card) {
    return null;
  }

  layout = document.createElement("div");
  layout.id = layoutId;
  applySideLayoutStyleV2F2(layout);

  card.insertAdjacentElement("beforebegin", layout);

  layout.appendChild(card);
  layout.appendChild(panel);

  card.style.margin = "0";
  card.style.width = "100%";

  panel.style.marginTop = "0";
  panel.style.height = "fit-content";

  return layout;
}

function ensureSideConfigLayoutV2F2() {
  forcePanelBesideChartV2F2("starConfigPanelV2F", "starTabLayoutV2F2");
  forcePanelBesideChartV2F2("colorConfigPanelV2F", "colorTabLayoutV2F2");
}

function getStarTabPanelV2F2() {
  return document.getElementById("starTabLayoutV2F2") ||
         document.getElementById("starTabLayoutV2F1") ||
         cardForCanvasV2D2("starChart");
}

function getColorTabPanelV2F2() {
  return document.getElementById("colorTabLayoutV2F2") ||
         document.getElementById("colorTabLayoutV2F1") ||
         cardForCanvasV2D2("colorChart");
}


// -----------------------------------------------------------------------------
// Dashboard v2f.3 move original config card to right
// Keep the config panel internal layout unchanged.
// -----------------------------------------------------------------------------

function findChartCardByTitleV2F3(canvasId, titleText, otherCanvasId) {
  const canvas = document.getElementById(canvasId);
  const other = document.getElementById(otherCanvasId);

  if (!canvas) {
    return null;
  }

  let cur = canvas.parentElement;

  while (cur && cur !== document.body) {
    const text = cur.textContent || "";
    const hasTitle = text.includes(titleText);
    const hasOther = other && cur.contains(other);

    if (hasTitle && !hasOther) {
      return cur;
    }

    cur = cur.parentElement;
  }

  return cardForCanvasV2D2(canvasId);
}

function starChartCardV2F3() {
  return findChartCardByTitleV2F3(
    "starChart",
    "最新 Star Catcher 表现",
    "colorChart"
  );
}

function colorChartCardV2F3() {
  return findChartCardByTitleV2F3(
    "colorChart",
    "最新 Color-Go 表现",
    "starChart"
  );
}

function applyRightLayoutStyleV2F3(layout) {
  layout.style.cssText = [
    "display:grid",
    "grid-template-columns:minmax(0,1fr) minmax(520px,620px)",
    "gap:20px",
    "align-items:start",
    "width:100%",
    "margin:0"
  ].join(";");
}

function moveOriginalConfigToRightV2F3(chartCard, panelId, layoutId) {
  const panel = document.getElementById(panelId);

  if (!chartCard || !panel) {
    return null;
  }

  let layout = document.getElementById(layoutId);

  if (!layout) {
    layout = document.createElement("div");
    layout.id = layoutId;
    applyRightLayoutStyleV2F3(layout);

    chartCard.insertAdjacentElement("beforebegin", layout);
  } else {
    applyRightLayoutStyleV2F3(layout);
  }

  if (chartCard.parentElement !== layout) {
    layout.appendChild(chartCard);
  }

  if (panel.parentElement !== layout) {
    layout.appendChild(panel);
  }

  chartCard.style.width = "100%";
  chartCard.style.margin = "0";

  panel.style.width = "100%";
  panel.style.maxWidth = "none";
  panel.style.margin = "0";
  panel.style.alignSelf = "start";

  return layout;
}

function ensureSideConfigLayoutV2F3() {
  moveOriginalConfigToRightV2F3(
    starChartCardV2F3(),
    "starConfigPanelV2F",
    "starTabLayoutV2F3"
  );

  moveOriginalConfigToRightV2F3(
    colorChartCardV2F3(),
    "colorConfigPanelV2F",
    "colorTabLayoutV2F3"
  );
}

function getStarTabPanelV2F3() {
  return document.getElementById("starTabLayoutV2F3") || starChartCardV2F3();
}

function getColorTabPanelV2F3() {
  return document.getElementById("colorTabLayoutV2F3") || colorChartCardV2F3();
}

async function loadDashboard() {
  try {
    ensureMainTabsV2D2();
    ensureLocalRangeControlsV2C1();
    ensureParamPlaceholdersV2D2();
    ensureSideConfigLayoutV2F3();
    ensureOverallZoneV2D2();
    ensureOverallRangeControlsV2D2();

    const starRange = starRangeV2C1;
    const colorRange = colorRangeV2C1;
    const tableRange = tableRangeV2C1;
    const overallRange = overallRangeV2D2;

    setText("cloudStatus", "读取中");
    setText("statusNote", "正在读取当前专区数据。");

    const [
      tableRecordsRes,
      starRecordsRes,
      colorRecordsRes,
      overallRecordsRes,
      starTrendRes,
      colorTrendRes,
      overallTrendRes
    ] = await Promise.all([
      fetch("/api/airtouch/records?type=all&range=" + encodeURIComponent(tableRange) + "&limit=50", { cache: "no-store" }),
      fetch("/api/airtouch/records?type=star&range=" + encodeURIComponent(starRange) + "&limit=200", { cache: "no-store" }),
      fetch("/api/airtouch/records?type=color_go&range=" + encodeURIComponent(colorRange) + "&limit=200", { cache: "no-store" }),
      fetch("/api/airtouch/records?type=all&range=" + encodeURIComponent(overallRange) + "&limit=500", { cache: "no-store" }),
      fetch("/api/airtouch/trends?type=star&range=" + encodeURIComponent(starRange), { cache: "no-store" }),
      fetch("/api/airtouch/trends?type=color_go&range=" + encodeURIComponent(colorRange), { cache: "no-store" }),
      fetch("/api/airtouch/trends?type=all&range=" + encodeURIComponent(overallRange), { cache: "no-store" })
    ]);

    if (
      !tableRecordsRes.ok ||
      !starRecordsRes.ok ||
      !colorRecordsRes.ok ||
      !overallRecordsRes.ok ||
      !starTrendRes.ok ||
      !colorTrendRes.ok ||
      !overallTrendRes.ok
    ) {
      throw new Error("API response failed");
    }

    const tableBody = await tableRecordsRes.json();
    const starBody = await starRecordsRes.json();
    const colorBody = await colorRecordsRes.json();
    const overallBody = await overallRecordsRes.json();
    const starTrendBody = await starTrendRes.json();
    const colorTrendBody = await colorTrendRes.json();
    const overallTrendBody = await overallTrendRes.json();

    const tableRecords = tableBody.records || [];
    const starRecords = starBody.records || [];
    const colorRecords = colorBody.records || [];
    const overallRecords = overallBody.records || [];

    setText("cloudStatus", "在线");
    setText("statusNote", "已按专区组织数据：概述 / Star Catcher / Color-Go / 综合数据。");

    setText("totalRecords", tableRecords.length);
    setText("starCount", starRecords.length);
    setText("colorCount", colorRecords.length);

    const newest = tableRecords[0] || starRecords[0] || colorRecords[0] || overallRecords[0];
    setText("lastSync", newest ? shortTime(newest.created_at) : "--");

    setText(
      "recordCountPill",
      dashboardRangeLabelV2C1(tableRange) + " · " + tableRecords.length + " records"
    );

    const latestStar = latestPayloadByTypeV2C1(starRecords, "star");
    const latestColor = latestPayloadByTypeV2C1(colorRecords, "color_go");

    setText("latestStarTime", latestStar ? formatTime(latestStar.created_at) : "--");
    setText("starHits", latestStar?.payload?.hits ?? "--");
    setText("starAvg", latestStar?.payload?.avg_ms ?? "--");
    setText("starFastest", latestStar?.payload?.fastest_ms ?? "--");
    setText("starScore", latestStar?.payload?.hit_score ?? "--");

    setText("latestColorTime", latestColor ? formatTime(latestColor.created_at) : "--");
    setText("colorAccuracy", latestColor?.payload?.accuracy ?? "--");
    setText("colorInhibit", latestColor?.payload?.inhibition ?? "--");
    setText("colorAvg", latestColor?.payload?.avg_ms ?? "--");
    setText("colorSpeed", latestColor?.payload?.speed_score ?? "--");

    renderTable(tableRecords);
    updateOverallCardsV2D2(overallRecords);

    const starTrend = starTrendBody?.series?.star || [];
    const colorTrend = colorTrendBody?.series?.color_go || [];
    const overallTrend = overallTrendBody?.series?.overall || [];

    const starChart = buildStarChartDataV2C1(starTrend, starRecords, starRange);
    const colorChart = buildColorChartDataV2C1(colorTrend, colorRecords, colorRange);
    const overallChart = buildOverallChartDataV2D2(overallTrend, overallRecords, overallRange);

    applyDashboardTabV2D2();

    drawLineChart(
      $("starChart"),
      [
        {
          name: "命中数",
          data: starChart.hits,
          colorA: "#ffd166",
          colorB: "#ffea9f"
        },
        {
          name: "命中评分",
          data: starChart.score,
          colorA: "#62a8ff",
          colorB: "#61f4ff"
        }
      ],
      starChart.labels,
      { min: 0, max: 100 }
    );

    drawLineChart(
      $("colorChart"),
      [
        {
          name: "准确率",
          data: colorChart.accuracy,
          colorA: "#67f5a6",
          colorB: "#9dffd0"
        },
        {
          name: "抑制控制",
          data: colorChart.inhibition,
          colorA: "#b388ff",
          colorB: "#d7c1ff"
        }
      ],
      colorChart.labels,
      { min: 0, max: 100 }
    );

    drawLineChart(
      $("overallChartV2D2"),
      [
        {
          name: "综合评分",
          data: overallChart.score,
          colorA: "#67f5a6",
          colorB: "#61f4ff"
        },
        {
          name: "平均反应/20",
          data: overallChart.avgMs.map(v => Math.min(100, Math.round(v / 20))),
          colorA: "#ffd166",
          colorB: "#ffea9f"
        }
      ],
      overallChart.labels,
      { min: 0, max: 100 }
    );

  } catch (e) {
    console.error(e);
    setText("cloudStatus", "异常");
    setText("statusNote", "读取失败，请检查 Worker / D1 API。");
  }
}
function updateClock() {
  const now = new Date();
  setText("clock", "本地时间：" + fmt.format(now));
}

window.addEventListener("resize", () => {
  clearTimeout(window.__resizeTimer);
  window.__resizeTimer = setTimeout(loadDashboard, 180);
});

updateClock();
setInterval(updateClock, 1000);

loadDashboard();
setInterval(loadDashboard, 20000);
</script>
</body>
</html>`;
}


// -----------------------------------------------------------------------------
// Dashboard v2f.4 clean rebuild
//
// Clean, static tab dashboard.
// No DOM re-parenting, no old layout mutation, no flicker.
// -----------------------------------------------------------------------------

function dashboardHtmlCleanV2F4() {
  return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>AirTouch Cloud Dashboard</title>
  <style>
    :root {
      --bg0:#07111f;
      --bg1:#101b31;
      --card:rgba(255,255,255,.08);
      --card2:rgba(255,255,255,.12);
      --line:rgba(255,255,255,.12);
      --text:#edf6ff;
      --muted:rgba(237,246,255,.62);
      --green:#67f5a6;
      --cyan:#61f4ff;
      --yellow:#ffe082;
      --purple:#c7a7ff;
    }
    * { box-sizing:border-box; }
    body {
      margin:0;
      min-height:100vh;
      background:
        linear-gradient(rgba(255,255,255,.035) 1px,transparent 1px),
        linear-gradient(90deg,rgba(255,255,255,.035) 1px,transparent 1px),
        radial-gradient(circle at 15% 0%,rgba(97,244,255,.12),transparent 34%),
        radial-gradient(circle at 72% 20%,rgba(199,167,255,.13),transparent 38%),
        #07111f;
      background-size:44px 44px,44px 44px,auto,auto,auto;
      color:var(--text);
      font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",sans-serif;
    }
    .wrap { max-width:1480px; margin:0 auto; padding:28px 28px 46px; }
    .hero {
      padding:26px;
      border:1px solid var(--line);
      border-radius:28px;
      background:linear-gradient(145deg,rgba(255,255,255,.09),rgba(255,255,255,.04));
      box-shadow:0 24px 70px rgba(0,0,0,.25);
    }
    .eyebrow { color:var(--green); font-weight:950; letter-spacing:.08em; font-size:13px; }
    h1 { margin:8px 0 8px; font-size:34px; }
    .sub { color:var(--muted); line-height:1.7; font-size:15px; }
    .tabs {
      display:flex;
      gap:10px;
      flex-wrap:wrap;
      margin:22px 0 18px;
      padding:10px;
      border:1px solid var(--line);
      border-radius:999px;
      background:rgba(255,255,255,.055);
    }
    .tab {
      border:1px solid rgba(255,255,255,.14);
      border-radius:999px;
      padding:10px 18px;
      background:rgba(255,255,255,.07);
      color:rgba(237,246,255,.82);
      font-weight:950;
      cursor:pointer;
    }
    .tab.active {
      color:#07111f;
      background:linear-gradient(135deg,var(--green),var(--cyan));
      box-shadow:0 10px 24px rgba(97,244,255,.18);
    }
    .section { display:none; }
    .section.active { display:block; }
    .grid4 { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:16px; }
    .card {
      border:1px solid var(--line);
      border-radius:26px;
      background:linear-gradient(145deg,rgba(255,255,255,.09),rgba(255,255,255,.045));
      padding:22px;
      box-shadow:0 18px 50px rgba(0,0,0,.18);
    }
    .kpi-title { color:var(--muted); font-size:14px; font-weight:800; }
    .kpi-value { margin-top:10px; font-size:36px; font-weight:950; }
    .unit { font-size:15px; margin-left:5px; color:rgba(237,246,255,.72); }
    .two-col {
      display:grid;
      grid-template-columns:minmax(0,1fr) minmax(520px,600px);
      gap:20px;
      align-items:start;
    }
    .title-row { display:flex; justify-content:space-between; align-items:flex-start; gap:14px; flex-wrap:wrap; }
    h2 { margin:0; font-size:26px; }
    .desc { margin-top:5px; color:var(--muted); font-size:14px; line-height:1.6; }
    .pill {
      padding:8px 13px;
      border-radius:999px;
      border:1px solid rgba(255,255,255,.12);
      background:rgba(255,255,255,.08);
      color:rgba(237,246,255,.76);
      font-weight:850;
      white-space:nowrap;
    }
    .mini-grid { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:12px; margin-top:16px; }
    .mini {
      padding:14px;
      border-radius:18px;
      border:1px solid rgba(255,255,255,.10);
      background:rgba(255,255,255,.075);
    }
    .mini .label { color:var(--muted); font-size:13px; }
    .mini .num { margin-top:6px; font-size:28px; font-weight:950; }
    .chart-box {
      margin-top:16px;
      padding:16px;
      border-radius:22px;
      background:rgba(7,17,31,.46);
    }
    canvas.chart { width:100%; height:300px; display:block; }
    .ranges { display:flex; align-items:center; gap:8px; flex-wrap:wrap; margin-top:14px; }
    .range-label { color:var(--muted); font-size:13px; font-weight:900; }
    .range-btn {
      border:1px solid rgba(255,255,255,.14);
      border-radius:999px;
      padding:7px 12px;
      color:rgba(237,246,255,.8);
      background:rgba(255,255,255,.07);
      font-weight:900;
      cursor:pointer;
    }
    .range-btn.active {
      color:#07111f;
      background:linear-gradient(135deg,var(--green),var(--cyan));
    }
    .config {
      border:1px solid rgba(103,245,166,.28);
      background:rgba(103,245,166,.06);
    }
    .form-preview {
      display:grid;
      grid-template-columns:minmax(0,1fr) 260px;
      gap:18px;
      align-items:start;
      margin-top:16px;
    }
    .row { margin-top:14px; }
    .row-head { display:flex; justify-content:space-between; color:rgba(237,246,255,.72); font-size:13px; }
    input[type=range] { width:100%; margin-top:8px; }
    input[type=checkbox] { width:18px; height:18px; }
    .check { display:flex; align-items:center; gap:10px; margin-top:14px; color:rgba(237,246,255,.76); font-size:13px; }
    .preview {
      padding:14px;
      border-radius:18px;
      border:1px solid rgba(255,255,255,.10);
      background:rgba(7,17,31,.38);
    }
    canvas.preview-canvas { width:100%; height:170px; border-radius:16px; background:rgba(0,0,0,.16); display:block; margin-top:8px; }
    .btn-row { display:flex; gap:10px; align-items:center; flex-wrap:wrap; margin-top:16px; }
    .primary {
      border:0;
      border-radius:999px;
      padding:11px 18px;
      color:#07111f;
      background:linear-gradient(135deg,var(--green),var(--cyan));
      font-weight:950;
      cursor:pointer;
    }
    .ghost {
      border:1px solid rgba(255,255,255,.15);
      border-radius:999px;
      padding:10px 15px;
      color:rgba(237,246,255,.82);
      background:rgba(255,255,255,.07);
      font-weight:900;
      cursor:pointer;
    }
    .status { color:var(--muted); font-size:13px; }
    table { width:100%; border-collapse:collapse; margin-top:14px; overflow:hidden; border-radius:16px; }
    th,td { padding:13px 12px; border-bottom:1px solid rgba(255,255,255,.08); text-align:left; font-size:14px; }
    th { color:rgba(237,246,255,.62); background:rgba(255,255,255,.05); }
    .badge { display:inline-flex; border-radius:999px; padding:5px 10px; background:rgba(255,255,255,.08); font-weight:900; }
    .badge.star { color:var(--yellow); }
    .badge.color { color:var(--green); }
    .footer { margin:28px 0 0; text-align:center; color:rgba(237,246,255,.38); line-height:1.7; font-weight:800; }
    @media (max-width:1180px) {
      .two-col { grid-template-columns:1fr; }
      .form-preview { grid-template-columns:1fr; }
      .grid4,.mini-grid { grid-template-columns:repeat(2,minmax(0,1fr)); }
    }
  </style>
</head>
<body>
  <main class="wrap">
    <section class="hero">
      <div class="eyebrow">AIRTOUCH CLOUD · LIVE</div>
      <h1>儿童空中无接触交互训练云端看板</h1>
      <div class="sub">P4 训练采集 → SD 本地档案 → ESP8266 网关 → Cloudflare Worker → D1 云数据库。当前版本采用 Tab 式专区结构，后续参数下发入口放在对应游戏专区内。</div>
    </section>

    <nav class="tabs">
      <button class="tab active" data-tab="overview">概述</button>
      <button class="tab" data-tab="star">Star Catcher</button>
      <button class="tab" data-tab="color">Color-Go</button>
      <button class="tab" data-tab="overall">综合数据</button>
    </nav>

    <section id="overview" class="section active">
      <div class="grid4">
        <div class="card"><div class="kpi-title">总训练记录</div><div class="kpi-value"><span id="kTotal">--</span><span class="unit">次</span></div></div>
        <div class="card"><div class="kpi-title">Star Catcher</div><div class="kpi-value"><span id="kStar">--</span><span class="unit">轮</span></div></div>
        <div class="card"><div class="kpi-title">Color-Go</div><div class="kpi-value"><span id="kColor">--</span><span class="unit">轮</span></div></div>
        <div class="card"><div class="kpi-title">最近同步</div><div class="kpi-value" id="kLast">--</div></div>
      </div>

      <div class="card" style="margin-top:18px;">
        <div class="title-row">
          <div><h2>最近训练记录</h2><div class="desc">实时展示 P4 训练后同步到云端的 Star / Color-Go 记录。</div></div>
          <div class="pill" id="recordPill">-- records</div>
        </div>
        <div class="ranges" id="tableRanges"></div>
        <table>
          <thead><tr><th>ID</th><th>类型</th><th>轮次</th><th>核心指标</th><th>反应时间</th><th>来源</th><th>云端时间</th></tr></thead>
          <tbody id="recordRows"></tbody>
        </table>
      </div>
    </section>

    <section id="star" class="section">
      <div class="two-col">
        <div class="card">
          <div class="title-row">
            <div><h2>最新 Star Catcher 表现</h2><div class="desc">命中数量、反应速度与综合训练表现。</div></div>
            <div class="pill" id="starTime">--</div>
          </div>
          <div class="mini-grid">
            <div class="mini"><div class="label">命中数</div><div class="num"><span id="starHits">--</span></div></div>
            <div class="mini"><div class="label">平均反应</div><div class="num"><span id="starAvg">--</span><span class="unit">ms</span></div></div>
            <div class="mini"><div class="label">最快反应</div><div class="num"><span id="starFast">--</span><span class="unit">ms</span></div></div>
            <div class="mini"><div class="label">命中评分</div><div class="num"><span id="starScore">--</span><span class="unit">分</span></div></div>
          </div>
          <div class="chart-box"><canvas id="starChartClean" class="chart"></canvas></div>
          <div class="ranges" id="starRanges"></div>
        </div>

        <div class="card config">
          <div class="title-row">
            <div><h2 style="color:var(--green);">Star Catcher 参数设置</h2><div class="desc">保存后进入云端参数表，后续由 ESP8266 / P4 拉取并写入 SD CONFIG.TXT。</div></div>
            <button class="ghost" onclick="clearToken()">清除 Token</button>
          </div>
          <div class="form-preview">
            <div>
              <div class="row"><div class="row-head"><span>目标大小</span><b><span id="starTargetV">--</span> px</b></div><input id="starTarget" type="range" min="36" max="90" step="1"></div>
              <div class="row"><div class="row-head"><span>悬停确认时间</span><b><span id="starDwellV">--</span> ms</b></div><input id="starDwell" type="range" min="250" max="1200" step="10"></div>
              <div class="row"><div class="row-head"><span>训练时长</span><b><span id="starDurationV">--</span> s</b></div><input id="starDuration" type="range" min="30" max="90" step="5"></div>
              <div class="row"><div class="row-head"><span>训练强度</span><b><span id="starIntensityV">--</span></b></div><input id="starIntensity" type="range" min="1" max="5" step="1"></div>
              <label class="check"><input id="starAdaptive" type="checkbox">启用自适应难度</label>
            </div>
            <div class="preview">
              <b>效果预览</b>
              <canvas id="starPreview" class="preview-canvas" width="240" height="170"></canvas>
              <div class="desc">圆越大越容易命中；悬停时间越长，确认越稳但反应更慢。</div>
            </div>
          </div>
          <div class="btn-row"><button class="primary" onclick="saveConfig()">保存 Star 参数到云端</button><button class="ghost" onclick="loadConfig(true)">重新读取</button><span class="status" id="starCfgStatus">--</span></div>
        </div>
      </div>
    </section>

    <section id="color" class="section">
      <div class="two-col">
        <div class="card">
          <div class="title-row">
            <div><h2>最新 Color-Go 表现</h2><div class="desc">准确率、抑制控制与选择反应速度。</div></div>
            <div class="pill" id="colorTime">--</div>
          </div>
          <div class="mini-grid">
            <div class="mini"><div class="label">准确率</div><div class="num"><span id="colorAcc">--</span><span class="unit">%</span></div></div>
            <div class="mini"><div class="label">抑制控制</div><div class="num"><span id="colorInhibit">--</span><span class="unit">分</span></div></div>
            <div class="mini"><div class="label">平均反应</div><div class="num"><span id="colorAvg">--</span><span class="unit">ms</span></div></div>
            <div class="mini"><div class="label">速度评分</div><div class="num"><span id="colorSpeed">--</span><span class="unit">分</span></div></div>
          </div>
          <div class="chart-box"><canvas id="colorChartClean" class="chart"></canvas></div>
          <div class="ranges" id="colorRanges"></div>
        </div>

        <div class="card config">
          <div class="title-row">
            <div><h2 style="color:var(--green);">Color-Go 参数设置</h2><div class="desc">保存后进入云端参数表，后续由 ESP8266 / P4 拉取并写入 SD CONFIG.TXT。</div></div>
            <button class="ghost" onclick="clearToken()">清除 Token</button>
          </div>
          <div class="form-preview">
            <div>
              <div class="row"><div class="row-head"><span>目标大小</span><b><span id="colorTargetV">--</span> px</b></div><input id="colorTarget" type="range" min="36" max="90" step="1"></div>
              <div class="row"><div class="row-head"><span>悬停确认时间</span><b><span id="colorDwellV">--</span> ms</b></div><input id="colorDwell" type="range" min="300" max="1200" step="10"></div>
              <div class="row"><div class="row-head"><span>气泡数量</span><b><span id="colorBubbleV">--</span> 个</b></div><input id="colorBubble" type="range" min="3" max="8" step="1"></div>
              <div class="row"><div class="row-head"><span>No-Go 比例</span><b><span id="colorNogoV">--</span> %</b></div><input id="colorNogo" type="range" min="10" max="50" step="5"></div>
              <div class="row"><div class="row-head"><span>训练时长</span><b><span id="colorDurationV">--</span> s</b></div><input id="colorDuration" type="range" min="30" max="90" step="5"></div>
              <div class="row"><div class="row-head"><span>训练强度</span><b><span id="colorIntensityV">--</span></b></div><input id="colorIntensity" type="range" min="1" max="5" step="1"></div>
            </div>
            <div class="preview">
              <b>效果预览</b>
              <canvas id="colorPreview" class="preview-canvas" width="240" height="170"></canvas>
              <div class="desc">紫色为 No-Go，绿色为 Go；No-Go 比例越高，抑制控制训练越强。</div>
            </div>
          </div>
          <div class="btn-row"><button class="primary" onclick="saveConfig()">保存 Color-Go 参数到云端</button><button class="ghost" onclick="loadConfig(true)">重新读取</button><span class="status" id="colorCfgStatus">--</span></div>
        </div>
      </div>
    </section>

    <section id="overall" class="section">
      <div class="card">
        <div class="title-row">
          <div><h2>综合数据</h2><div class="desc">融合 Star Catcher 与 Color-Go，展示总体训练活跃度、综合表现和长期成长趋势。</div></div>
          <div class="pill" id="overallPill">周 · 综合</div>
        </div>
        <div class="mini-grid">
          <div class="mini"><div class="label">训练次数</div><div class="num"><span id="overallTotal">--</span><span class="unit">次</span></div></div>
          <div class="mini"><div class="label">综合评分</div><div class="num"><span id="overallScore">--</span><span class="unit">分</span></div></div>
          <div class="mini"><div class="label">平均反应</div><div class="num"><span id="overallAvg">--</span><span class="unit">ms</span></div></div>
          <div class="mini"><div class="label">最好表现</div><div class="num"><span id="overallBest">--</span><span class="unit">分</span></div></div>
        </div>
        <div class="chart-box"><canvas id="overallChartClean" class="chart"></canvas></div>
        <div class="ranges" id="overallRanges"></div>
      </div>
    </section>

    <div class="footer">AirTouch 智训板 · 端侧采集 — 本地档案 — 云端同步 — 可视化评估<br/>Dashboard v2f.4 clean · Cloudflare Worker + D1</div>
  </main>

<script>
var workerState = {
  tab:"overview",
  tableRange:"week",
  starRange:"week",
  colorRange:"week",
  overallRange:"week",
  cfg:null
};

var rangeOptions = [["day","日"],["week","周"],["month","月"],["year","年"]];

function el(id){ return document.getElementById(id); }
function setText(id,v){ var x=el(id); if(x){ x.textContent = v; } }
function num(v){ var n=Number(v); return Number.isFinite(n) ? n : 0; }
function shortTime(s){ if(!s){return "--";} var d=new Date(s); if(isNaN(d.getTime())){return "--";} return String(d.getMonth()+1).padStart(2,"0")+"/"+String(d.getDate()).padStart(2,"0")+" "+String(d.getHours()).padStart(2,"0")+":"+String(d.getMinutes()).padStart(2,"0"); }
function api(path){ return fetch(path,{cache:"no-store"}).then(function(r){ if(!r.ok){throw new Error(path);} return r.json(); }); }

function scheduleChartRedraw(reason){
  setTimeout(function(){
    requestAnimationFrame(function(){
      requestAnimationFrame(function(){
        drawAllCharts();
      });
    });
  }, 80);
}


function setTab(tab){
  workerState.tab = tab;
  document.querySelectorAll(".tab").forEach(function(b){ b.classList.toggle("active", b.dataset.tab === tab); });
  document.querySelectorAll(".section").forEach(function(s){ s.classList.toggle("active", s.id === tab); });
  scheduleChartRedraw("tab");
}

document.querySelectorAll(".tab").forEach(function(b){ b.addEventListener("click", function(){ setTab(b.dataset.tab); }); });

function renderRanges(hostId, active, onClick){
  var host = el(hostId);
  if(!host){return;}
  host.innerHTML = '<span class="range-label">范围</span>';
  rangeOptions.forEach(function(item){
    var b=document.createElement("button");
    b.className="range-btn"+(item[0]===active?" active":"");
    b.textContent=item[1];
    b.onclick=function(){ onClick(item[0]); };
    host.appendChild(b);
  });
}

function pointLabel(r, range){
  if(range==="day"){ return shortTime(r.created_at); }
  return "rid " + r.rid;
}

function sortAsc(rows){
  return rows.slice().sort(function(a,b){
    var ta=Date.parse(a.created_at||"");
    var tb=Date.parse(b.created_at||"");
    if(Number.isFinite(ta) && Number.isFinite(tb) && ta!==tb){ return ta-tb; }
    return num(a.rid)-num(b.rid);
  });
}

function drawLine(canvas, series, labels){
  if(!canvas){return;}

  var ctx = canvas.getContext("2d");
  var rect = canvas.getBoundingClientRect();
  var dpr = window.devicePixelRatio || 1;

  var cssW = Math.max(320, Math.floor(rect.width || canvas.clientWidth || 720));
  var cssH = Math.max(220, Math.floor(rect.height || canvas.clientHeight || 300));

  canvas.width = Math.floor(cssW * dpr);
  canvas.height = Math.floor(cssH * dpr);

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssW, cssH);

  var W = cssW;
  var H = cssH;
  var padL = 52;
  var padR = 30;
  var padT = 28;
  var padB = 42;
  var innerW = W - padL - padR;
  var innerH = H - padT - padB;

  labels = Array.isArray(labels) ? labels.slice() : [];
  series = Array.isArray(series) ? series : [];

  var n = labels.length;
  series.forEach(function(s){
    if(Array.isArray(s.data)){
      n = Math.max(n, s.data.length);
    }
  });

  if(labels.length < n){
    for(var li = labels.length; li < n; li++){
      labels.push(String(li + 1));
    }
  }

  // Background
  ctx.fillStyle = "rgba(0,0,0,.08)";
  ctx.fillRect(0, 0, W, H);

  // Grid + y labels
  ctx.lineWidth = 1;
  ctx.font = "12px system-ui";
  ctx.textBaseline = "middle";

  for(var i = 0; i <= 4; i++){
    var v = i * 25;
    var y = padT + innerH * (1 - v / 100);

    ctx.strokeStyle = "rgba(237,246,255,.13)";
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(W - padR, y);
    ctx.stroke();

    ctx.fillStyle = "rgba(237,246,255,.58)";
    ctx.fillText(String(v), 12, y);
  }

  if(n <= 0){
    ctx.fillStyle = "rgba(237,246,255,.55)";
    ctx.font = "15px system-ui";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText("暂无趋势数据，完成训练后自动生成折线图", W / 2, H / 2);
    ctx.textAlign = "left";
    return;
  }

  function xAt(i){
    if(n <= 1){
      return padL + innerW * 0.5;
    }
    return padL + innerW * i / (n - 1);
  }

  function yAt(v){
    var value = Math.max(0, Math.min(100, num(v)));
    return padT + innerH * (1 - value / 100);
  }

  var colors = ["#61f4ff", "#ffe082", "#c7a7ff", "#67f5a6"];

  series.forEach(function(s, si){
    var data = Array.isArray(s.data) ? s.data.map(num) : [];
    if(data.length === 0){
      return;
    }

    var color = colors[si % colors.length];

    // Soft area fill
    ctx.beginPath();
    data.forEach(function(v, i){
      var xx = xAt(i);
      var yy = yAt(v);
      if(i === 0){
        ctx.moveTo(xx, yy);
      }else{
        ctx.lineTo(xx, yy);
      }
    });
    ctx.lineTo(xAt(data.length - 1), padT + innerH);
    ctx.lineTo(xAt(0), padT + innerH);
    ctx.closePath();
    ctx.fillStyle = color + "22";
    ctx.fill();

    // Main line
    ctx.beginPath();
    data.forEach(function(v, i){
      var xx = xAt(i);
      var yy = yAt(v);
      if(i === 0){
        ctx.moveTo(xx, yy);
      }else{
        ctx.lineTo(xx, yy);
      }
    });
    ctx.strokeStyle = color;
    ctx.lineWidth = 4;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();

    // Points
    data.forEach(function(v, i){
      var xx = xAt(i);
      var yy = yAt(v);

      ctx.fillStyle = "rgba(7,17,31,.95)";
      ctx.beginPath();
      ctx.arc(xx, yy, 7, 0, Math.PI * 2);
      ctx.fill();

      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(xx, yy, 4.5, 0, Math.PI * 2);
      ctx.fill();
    });

    // Legend
    var lx = padL + si * 112;
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(lx, 14, 4.5, 0, Math.PI * 2);
    ctx.fill();

    ctx.fillStyle = "rgba(237,246,255,.76)";
    ctx.font = "12px system-ui";
    ctx.textBaseline = "middle";
    ctx.fillText(s.name || ("指标" + (si + 1)), lx + 10, 14);
  });

  // x labels
  ctx.fillStyle = "rgba(237,246,255,.55)";
  ctx.font = "12px system-ui";
  ctx.textBaseline = "alphabetic";
  ctx.textAlign = "left";
  ctx.fillText(labels[0] || "", padL, H - 12);

  if(labels.length > 1){
    ctx.textAlign = "right";
    ctx.fillText(labels[labels.length - 1] || "", W - padR, H - 12);
    ctx.textAlign = "left";
  }
}

function overallScore(r){
  var p=r.payload||{};
  var arr=[];
  if(r.type==="star"){ arr=[p.hit_score,p.speed_score]; }
  if(r.type==="color_go"){ arr=[p.accuracy,p.inhibition,p.speed_score]; }
  var xs=arr.map(num).filter(function(v){return v>0;});
  return xs.length ? xs.reduce(function(a,b){return a+b;},0)/xs.length : 0;
}

function updateRecordTable(rows){
  setText("recordPill", rangeLabel(workerState.tableRange)+" · "+rows.length+" records");
  var html=rows.map(function(r){
    var p=r.payload||{};
    var type=r.type==="star"?"Star Catcher":"Color-Go";
    var cls=r.type==="star"?"star":"color";
    var core=r.type==="star" ? ("命中 "+(p.hits||0)+" · 命中分 "+(p.hit_score||0)) : ("准确率 "+(p.accuracy||0)+"% · 抑制 "+(p.inhibition||0));
    return "<tr><td>#"+r.id+"</td><td><span class='badge "+cls+"'>"+type+"</span></td><td>rid "+r.rid+"</td><td>"+core+"</td><td>avg "+(p.avg_ms||0)+" ms · best "+(p.fastest_ms||0)+" ms</td><td>"+(r.source||"--")+"</td><td>"+shortTime(r.created_at)+"</td></tr>";
  }).join("");
  el("recordRows").innerHTML = html || "<tr><td colspan='7'>暂无记录</td></tr>";
}

function rangeLabel(r){ var m={day:"日",week:"周",month:"月",year:"年"}; return m[r]||"周"; }

var latest = { table:[], star:[], color:[], overall:[] };

function loadDashboard(){
  renderRanges("tableRanges", workerState.tableRange, function(r){ workerState.tableRange=r; loadDashboard(); });
  renderRanges("starRanges", workerState.starRange, function(r){ workerState.starRange=r; loadDashboard(); });
  renderRanges("colorRanges", workerState.colorRange, function(r){ workerState.colorRange=r; loadDashboard(); });
  renderRanges("overallRanges", workerState.overallRange, function(r){ workerState.overallRange=r; loadDashboard(); });

  Promise.all([
    api("/api/airtouch/records?type=all&range="+workerState.tableRange+"&limit=50"),
    api("/api/airtouch/records?type=star&range="+workerState.starRange+"&limit=200"),
    api("/api/airtouch/records?type=color_go&range="+workerState.colorRange+"&limit=200"),
    api("/api/airtouch/records?type=all&range="+workerState.overallRange+"&limit=500")
  ]).then(function(all){
    latest.table=all[0].records||[];
    latest.star=all[1].records||[];
    latest.color=all[2].records||[];
    latest.overall=all[3].records||[];

    setText("kTotal", latest.table.length);
    setText("kStar", latest.star.length);
    setText("kColor", latest.color.length);
    setText("kLast", latest.table[0]?shortTime(latest.table[0].created_at):"--");

    var s=latest.star[0], sp=s?(s.payload||{}):{};
    setText("starTime", s?shortTime(s.created_at):"--");
    setText("starHits", sp.hits||"--");
    setText("starAvg", sp.avg_ms||"--");
    setText("starFast", sp.fastest_ms||"--");
    setText("starScore", sp.hit_score||"--");

    var c=latest.color[0], cp=c?(c.payload||{}):{};
    setText("colorTime", c?shortTime(c.created_at):"--");
    setText("colorAcc", cp.accuracy||"--");
    setText("colorInhibit", cp.inhibition||"--");
    setText("colorAvg", cp.avg_ms||"--");
    setText("colorSpeed", cp.speed_score||"--");

    updateRecordTable(latest.table);
    updateOverall();
    scheduleChartRedraw("load");
  }).catch(function(e){ console.error(e); });
}

function drawAllCharts(){
  function safeLabel(r, range){
    try {
      if(typeof pointLabel === "function"){
        return pointLabel(r, range);
      }
    } catch(e) {
      // fallback below
    }

    if(range === "day" && r.created_at){
      return shortTime(r.created_at);
    }

    return "rid " + (r.rid ?? "");
  }

  var sr = sortAsc(latest.star || []);
  drawLine(el("starChartClean"), [
    {name:"命中数", data:sr.map(function(r){return num((r.payload||{}).hits);})},
    {name:"命中评分", data:sr.map(function(r){return num((r.payload||{}).hit_score);})}
  ], sr.map(function(r){return safeLabel(r, workerState.starRange);}));

  var cr = sortAsc(latest.color || []);
  drawLine(el("colorChartClean"), [
    {name:"准确率", data:cr.map(function(r){return num((r.payload||{}).accuracy);})},
    {name:"抑制控制", data:cr.map(function(r){return num((r.payload||{}).inhibition);})}
  ], cr.map(function(r){return safeLabel(r, workerState.colorRange);}));

  var or = sortAsc(latest.overall || []);
  drawLine(el("overallChartClean"), [
    {name:"综合评分", data:or.map(function(r){return Math.round(overallScore(r));})},
    {name:"平均反应/20", data:or.map(function(r){return Math.min(100, Math.round(num((r.payload||{}).avg_ms) / 20));})}
  ], or.map(function(r){return safeLabel(r, workerState.overallRange);}));
}

function updateOverall(){
  var rows=latest.overall;
  var scores=rows.map(overallScore).filter(function(v){return v>0;});
  var avg=scores.length?scores.reduce(function(a,b){return a+b;},0)/scores.length:0;
  var best=scores.length?Math.max.apply(null,scores):0;
  var avgMs=rows.map(function(r){return num((r.payload||{}).avg_ms);}).filter(function(v){return v>0;});
  var ms=avgMs.length?avgMs.reduce(function(a,b){return a+b;},0)/avgMs.length:0;
  setText("overallPill", rangeLabel(workerState.overallRange)+" · 综合");
  setText("overallTotal", rows.length);
  setText("overallScore", avg?Math.round(avg):"--");
  setText("overallBest", best?Math.round(best):"--");
  setText("overallAvg", ms?Math.round(ms):"--");
}

function bindSlider(id,label,cb){
  var x=el(id);
  if(!x){return;}
  function upd(){ setText(label,x.value); if(cb){cb();} }
  x.addEventListener("input",upd); upd();
}

function defaultCfg(){
  return {star:{target_radius:56,dwell_ms:336,round_duration_s:45,difficulty:2,adaptive_enabled:true},color_go:{target_radius:54,dwell_ms:520,bubble_count:4,nogo_ratio:25,round_duration_s:45,difficulty:2,adaptive_enabled:false},global:{distance_guard_enabled:true,difficulty_floor:1,difficulty_ceiling:5}};
}

function fillCfg(cfg){
  cfg=cfg||defaultCfg();
  var s=cfg.star||defaultCfg().star, c=cfg.color_go||defaultCfg().color_go;
  el("starTarget").value=s.target_radius; el("starDwell").value=s.dwell_ms; el("starDuration").value=s.round_duration_s; el("starIntensity").value=s.difficulty; el("starAdaptive").checked=!!s.adaptive_enabled;
  el("colorTarget").value=c.target_radius; el("colorDwell").value=c.dwell_ms; el("colorBubble").value=c.bubble_count; el("colorNogo").value=c.nogo_ratio; el("colorDuration").value=c.round_duration_s; el("colorIntensity").value=c.difficulty;
  updateSliderLabels(); drawPreviews();
}

function updateSliderLabels(){
  [["starTarget","starTargetV"],["starDwell","starDwellV"],["starDuration","starDurationV"],["starIntensity","starIntensityV"],["colorTarget","colorTargetV"],["colorDwell","colorDwellV"],["colorBubble","colorBubbleV"],["colorNogo","colorNogoV"],["colorDuration","colorDurationV"],["colorIntensity","colorIntensityV"]].forEach(function(p){ setText(p[1],el(p[0]).value); });
}

function bindConfig(){
  ["starTarget","starDwell","starDuration","starIntensity","colorTarget","colorDwell","colorBubble","colorNogo","colorDuration","colorIntensity"].forEach(function(id){ bindSlider(id,id+"V",drawPreviews); });
  ["starAdaptive"].forEach(function(id){ el(id).addEventListener("change",drawPreviews); });
}

function loadConfig(force){
  api("/api/airtouch/config/latest?device_id=airtouch_001&user_id=child_001").then(function(r){
    workerState.cfg=r.config||defaultCfg();
    fillCfg(workerState.cfg);
    var msg=r.has_config?("云端版本 v"+r.config_version+"，设备已应用 v"+(r.applied_version||0)):"当前使用默认参数";
    setText("starCfgStatus",msg); setText("colorCfgStatus",msg);
  }).catch(function(){ fillCfg(defaultCfg()); });
}

function collectCfg(){
  return {cloud_config_enabled:true,
    star:{target_radius:num(el("starTarget").value),dwell_ms:num(el("starDwell").value),round_duration_s:num(el("starDuration").value),difficulty:num(el("starIntensity").value),adaptive_enabled:el("starAdaptive").checked},
    color_go:{target_radius:num(el("colorTarget").value),dwell_ms:num(el("colorDwell").value),bubble_count:num(el("colorBubble").value),nogo_ratio:num(el("colorNogo").value),round_duration_s:num(el("colorDuration").value),difficulty:num(el("colorIntensity").value),adaptive_enabled:false},
    global:{distance_guard_enabled:true,difficulty_floor:1,difficulty_ceiling:5}
  };
}

function token(){ return "airtouch_demo_token_2026"; }
function requireToken(){ return "airtouch_demo_token_2026"; }
function clearToken(){ localStorage.removeItem("airtouch_config_token_v2f"); setText("starCfgStatus","已清除 Token"); setText("colorCfgStatus","已清除 Token"); }

function saveConfig(){
  var t=requireToken(); if(!t){return;}
  var body={device_id:"airtouch_001",user_id:"child_001",config:collectCfg()};
  setText("starCfgStatus","正在保存..."); setText("colorCfgStatus","正在保存...");
  fetch("/api/airtouch/config/update",{method:"POST",headers:{"Content-Type":"application/json","X-AirTouch-Token": "airtouch_demo_token_2026"},body:JSON.stringify(body)})
    .then(function(r){return r.json().then(function(j){ if(!r.ok||!j.ok){throw new Error(j.error||"save failed");} return j;});})
    .then(function(j){ workerState.cfg=j.config; fillCfg(j.config); setText("starCfgStatus","保存成功：云端版本 v"+j.config_version+"，等待设备同步"); setText("colorCfgStatus","保存成功：云端版本 v"+j.config_version+"，等待设备同步"); })
    .catch(function(e){ setText("starCfgStatus","保存失败："+e.message); setText("colorCfgStatus","保存失败："+e.message); });
}

function drawStarPreview(){
  var cv=el("starPreview"),ctx=cv.getContext("2d"),w=cv.width,h=cv.height;
  var r=num(el("starTarget").value),d=num(el("starDwell").value),it=num(el("starIntensity").value);
  ctx.clearRect(0,0,w,h); ctx.fillStyle="rgba(255,255,255,.05)"; ctx.fillRect(0,0,w,h);
  ctx.strokeStyle="rgba(255,255,255,.12)"; for(var i=1;i<=3;i++){ctx.beginPath();ctx.arc(w/2,h/2,24*i,0,Math.PI*2);ctx.stroke();}
  ctx.fillStyle="rgba(255,209,102,.92)"; ctx.beginPath(); ctx.arc(w/2,h/2,Math.max(16,Math.min(62,r/1.25)),0,Math.PI*2); ctx.fill();
  ctx.fillStyle="rgba(97,244,255,.95)"; ctx.beginPath(); ctx.arc(w/2+48,h/2-38,7+it*2,0,Math.PI*2); ctx.fill();
  ctx.fillStyle="rgba(237,246,255,.72)"; ctx.font="12px system-ui"; ctx.fillText("target "+r+"px",14,24); ctx.fillText("dwell "+d+"ms",14,42); ctx.fillText("intensity "+it,14,60);
}

function drawColorPreview(){
  var cv=el("colorPreview"),ctx=cv.getContext("2d"),w=cv.width,h=cv.height;
  var count=num(el("colorBubble").value),nogo=num(el("colorNogo").value),r=num(el("colorTarget").value),d=num(el("colorDwell").value);
  ctx.clearRect(0,0,w,h); ctx.fillStyle="rgba(255,255,255,.05)"; ctx.fillRect(0,0,w,h);
  var ps=[[54,54],[122,45],[186,62],[75,122],[150,122],[205,118],[42,98],[118,88]];
  var ng=Math.max(1,Math.round(count*nogo/100));
  for(var i=0;i<count;i++){ var p=ps[i%ps.length],isN=i<ng; ctx.fillStyle=isN?"rgba(199,167,255,.92)":"rgba(103,245,166,.92)"; ctx.beginPath(); ctx.arc(p[0],p[1],Math.max(12,Math.min(34,r/2.1)),0,Math.PI*2); ctx.fill(); ctx.fillStyle="#07111f"; ctx.font="bold 12px system-ui"; ctx.fillText(isN?"N":"G",p[0]-4,p[1]+4); }
  ctx.fillStyle="rgba(237,246,255,.72)"; ctx.font="12px system-ui"; ctx.fillText(count+" bubbles · No-Go "+nogo+"%",14,24); ctx.fillText("dwell "+d+"ms",14,42);
}

function drawPreviews(){ updateSliderLabels(); drawStarPreview(); drawColorPreview(); }

bindConfig();
loadConfig(false);
loadDashboard();
setInterval(loadDashboard,20000);
window.addEventListener("resize", function(){ setTimeout(drawAllCharts,80); });
</script>
</body>
</html>`;
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === "OPTIONS") {
      return jsonResponse({ ok: true });
    }

    if (url.pathname === "/" || url.pathname === "/dashboard") {
      return htmlResponse(dashboardHtmlCleanV2F4());
    }

    if (url.pathname === "/health") {
      return textResponse("AirTouch Cloudflare Worker OK\n");
    }

    if (url.pathname === "/api/airtouch/records" && request.method === "GET") {
      return handleRecordsGet(request, env);
    }

    if (url.pathname === "/api/airtouch/records" && request.method === "POST") {
      return handleRecordPost(request, env);
    }

    if (url.pathname === "/api/airtouch/latest" && request.method === "GET") {
      return handleLatest(request, env);
    }

    if (url.pathname === "/api/airtouch/summary" && request.method === "GET") {
      return handleSummary(env);
    }

    if (url.pathname === "/api/airtouch/config/latest" && request.method === "GET") {
      return handleConfigLatestGet(request, env);
    }

    if (url.pathname === "/api/airtouch/config/update" && request.method === "POST") {
      return handleConfigUpdatePost(request, env);
    }

    if (url.pathname === "/api/airtouch/config/ack" && request.method === "POST") {
      return handleConfigAckPost(request, env);
    }

    if (url.pathname === "/api/airtouch/trends" && request.method === "GET") {
      return handleTrendsGet(request, env);
    }

    return jsonResponse({
      ok: false,
      error: "NOT_FOUND",
      path: url.pathname
    }, 404);
  }
};





































