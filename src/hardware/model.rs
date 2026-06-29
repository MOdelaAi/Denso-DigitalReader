//! Hardware spec domain type. Read-only and transient — collected fresh each
//! launch, never persisted (see [`super::collect`]).

/// Static hardware description shown in the System section.
pub struct HardwareSpec {
    pub os: String,
    pub device: String,
    pub ram: String,
    pub storage: String,
}
