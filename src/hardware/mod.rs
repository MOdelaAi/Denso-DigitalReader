//! Host hardware spec for the System section. Live, read-only.

mod model;
mod repo;

pub use model::HardwareSpec;
pub use repo::collect;
