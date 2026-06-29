//! Reader domain type. Mirrors the `readings` table (see
//! [`crate::db::migrations`]).

// Constructed only by the (future) inference writer and by tests until camera
// capture is wired; the dead-code allowance here is expected, not accidental.
#![allow(dead_code)]

/// A single decoded reading. `id` is 0 until persisted (assigned by the DB)
/// and populated when read back via [`super::recent`].
#[derive(Clone, Debug, PartialEq)]
pub struct Reading {
    pub id: i64,
    pub ts: i64, // unix milliseconds
    pub value: String,
    pub confidence: Option<f64>,
    pub image_path: Option<String>,
}
