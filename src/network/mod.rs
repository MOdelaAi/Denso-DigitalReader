#[cfg(windows)]
mod windows;
#[cfg(target_os = "linux")]
mod linux;

#[derive(Default, Clone, PartialEq, Debug)]
pub struct InterfaceStatus {
    pub connected: bool,
    pub ip: String,
    pub gateway: String,
    pub ssid: String,
    pub signal: String,
}

#[derive(Default, Clone, Debug)]
pub struct NetworkSnapshot {
    pub ethernet: InterfaceStatus,
    pub wifi: InterfaceStatus,
}

pub trait NetworkBackend {
    fn snapshot(&self) -> NetworkSnapshot;
}

struct NullBackend;
impl NetworkBackend for NullBackend {
    fn snapshot(&self) -> NetworkSnapshot {
        NetworkSnapshot::default()
    }
}

pub fn backend() -> Box<dyn NetworkBackend> {
    #[cfg(windows)]
    {
        return Box::new(windows::WindowsBackend);
    }
    #[cfg(target_os = "linux")]
    {
        return Box::new(linux::LinuxBackend);
    }
    #[allow(unreachable_code)]
    {
        Box::new(NullBackend)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn null_backend_is_all_disconnected() {
        let snap = NullBackend.snapshot();
        assert!(!snap.ethernet.connected);
        assert!(!snap.wifi.connected);
        assert_eq!(snap.ethernet.ip, "");
    }
}
