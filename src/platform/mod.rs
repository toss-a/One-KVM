//! Platform selection and capability reporting.

pub mod capabilities;
pub mod defaults;
#[cfg(target_os = "linux")]
pub mod linux;
#[cfg(unix)]
pub mod usb_reset;
#[cfg(windows)]
pub mod windows;

pub use capabilities::{FeatureCapability, PlatformCapabilities, PlatformMode};

#[cfg(target_os = "linux")]
pub use linux::is_rk35xx_or_newer;

#[cfg(not(target_os = "linux"))]
pub fn is_rk35xx_or_newer() -> bool {
    true
}
