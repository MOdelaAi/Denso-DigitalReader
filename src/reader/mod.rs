//! Digit-reader log. System-written, append-only: the app records each
//! reading; the UI may only view them. See [`repo`] for why no mutation API
//! is exposed.

mod model;
mod repo;

pub use model::Reading;
// Re-exported for the (future) inference writer and history UI; no caller yet.
#[allow(unused_imports)]
pub use repo::{insert, recent};
