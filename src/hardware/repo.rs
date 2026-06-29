//! Collects the host's hardware spec via `sysinfo`. Live, read-only: there is
//! no persistence here by design.

use super::HardwareSpec;
use sysinfo::{Disks, System};

pub fn collect() -> HardwareSpec {
    let mut sys = System::new();
    sys.refresh_memory();

    let os = match (System::name(), System::os_version()) {
        (Some(n), Some(v)) => format!("{n} {v}"),
        (Some(n), None) => n,
        (None, Some(v)) => v,
        (None, None) => "Unknown".to_string(),
    };

    let device = System::host_name().unwrap_or_else(|| "Unknown".to_string());

    let ram = size_or_unknown(sys.total_memory());

    let disks = Disks::new_with_refreshed_list();
    let total_storage: u64 = disks.list().iter().map(|d| d.total_space()).sum();
    let storage = size_or_unknown(total_storage);

    HardwareSpec { os, device, ram, storage }
}

fn format_bytes(bytes: u64) -> String {
    const GB: u64 = 1_000_000_000;
    const TB: u64 = 1_000_000_000_000;
    if bytes >= TB {
        format!("{:.1} TB", bytes as f64 / TB as f64)
    } else {
        format!("{} GB", bytes / GB)
    }
}

fn size_or_unknown(bytes: u64) -> String {
    if bytes == 0 {
        "Unknown".to_string()
    } else {
        format_bytes(bytes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_gigabytes_whole() {
        assert_eq!(format_bytes(16_000_000_000), "16 GB");
    }

    #[test]
    fn formats_gigabytes_rounded() {
        assert_eq!(format_bytes(512_110_190_592), "512 GB");
    }

    #[test]
    fn formats_terabytes_one_decimal() {
        assert_eq!(format_bytes(2_000_000_000_000), "2.0 TB");
    }

    #[test]
    fn formats_sub_gigabyte_as_zero_gb() {
        assert_eq!(format_bytes(500_000_000), "0 GB");
    }

    #[test]
    fn size_or_unknown_zero_is_unknown() {
        assert_eq!(size_or_unknown(0), "Unknown");
    }

    #[test]
    fn size_or_unknown_nonzero_formats() {
        assert_eq!(size_or_unknown(16_000_000_000), "16 GB");
    }
}
