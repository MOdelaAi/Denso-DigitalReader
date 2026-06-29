//! Digit-reader log. System-written, append-only: the app records each
//! reading; the UI may only view them. This module deliberately exposes no
//! update or delete over individual rows — the absence of that API *is* the
//! read-only-to-user policy (SQLite has no row-level grants). Retention
//! pruning, when needed, will be a separate system-only operation.

// The reader log is in place ahead of its writer: nothing inserts readings
// until camera capture + inference are wired. Until then these are exercised
// only by tests, so dead-code warnings here are expected, not accidental.
#![allow(dead_code)]

use rusqlite::Connection;

/// A single decoded reading. `id` is 0 until persisted (assigned by the DB)
/// and populated when read back via [`recent`].
#[derive(Clone, Debug, PartialEq)]
pub struct Reading {
    pub id: i64,
    pub ts: i64, // unix milliseconds
    pub value: String,
    pub confidence: Option<f64>,
    pub image_path: Option<String>,
}

/// Append a reading. Returns the new row id. The caller-supplied `id` is
/// ignored — the DB assigns it.
pub fn insert(conn: &Connection, r: &Reading) -> rusqlite::Result<i64> {
    conn.execute(
        "INSERT INTO readings (ts, value, confidence, image_path) \
         VALUES (?1, ?2, ?3, ?4)",
        rusqlite::params![r.ts, r.value, r.confidence, r.image_path],
    )?;
    Ok(conn.last_insert_rowid())
}

/// The `limit` most recent readings, newest first.
pub fn recent(conn: &Connection, limit: usize) -> rusqlite::Result<Vec<Reading>> {
    let mut stmt = conn.prepare(
        "SELECT id, ts, value, confidence, image_path FROM readings \
         ORDER BY ts DESC, id DESC LIMIT ?1",
    )?;
    let rows = stmt.query_map([limit], |row| {
        Ok(Reading {
            id: row.get(0)?,
            ts: row.get(1)?,
            value: row.get(2)?,
            confidence: row.get(3)?,
            image_path: row.get(4)?,
        })
    })?;
    rows.collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db() -> Connection {
        let c = Connection::open_in_memory().unwrap();
        crate::db::run_migrations(&c).unwrap();
        c
    }

    fn reading(ts: i64, value: &str) -> Reading {
        Reading { id: 0, ts, value: value.into(), confidence: Some(0.9), image_path: None }
    }

    #[test]
    fn insert_then_recent_returns_the_reading() {
        let c = db();
        let id = insert(&c, &reading(100, "0427")).unwrap();
        assert!(id > 0);

        let rows = recent(&c, 10).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].value, "0427");
        assert_eq!(rows[0].ts, 100);
        assert_eq!(rows[0].confidence, Some(0.9));
        assert_eq!(rows[0].id, id);
    }

    #[test]
    fn recent_orders_newest_first() {
        let c = db();
        insert(&c, &reading(100, "a")).unwrap();
        insert(&c, &reading(300, "c")).unwrap();
        insert(&c, &reading(200, "b")).unwrap();

        let values: Vec<String> = recent(&c, 10).unwrap().into_iter().map(|r| r.value).collect();
        assert_eq!(values, vec!["c", "b", "a"]);
    }

    #[test]
    fn recent_respects_limit() {
        let c = db();
        for ts in [10, 20, 30] {
            insert(&c, &reading(ts, "x")).unwrap();
        }
        let rows = recent(&c, 2).unwrap();
        assert_eq!(rows.len(), 2);
        assert_eq!((rows[0].ts, rows[1].ts), (30, 20));
    }

    #[test]
    fn confidence_and_image_path_roundtrip_when_absent() {
        let c = db();
        let r = Reading { id: 0, ts: 5, value: "1".into(), confidence: None, image_path: None };
        insert(&c, &r).unwrap();
        let back = &recent(&c, 1).unwrap()[0];
        assert_eq!(back.confidence, None);
        assert_eq!(back.image_path, None);
    }
}
