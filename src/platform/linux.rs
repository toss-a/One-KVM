//! Linux platform capabilities.

use std::sync::OnceLock;
use super::{FeatureCapability, PlatformCapabilities, PlatformMode};

pub fn capabilities() -> PlatformCapabilities {
    PlatformCapabilities {
        mode: PlatformMode::Linux,
        mode_label: PlatformMode::Linux.label(),
        video_capture: FeatureCapability::available(["v4l2"]),
        encoder: FeatureCapability::available([
            "software", "vaapi", "nvenc", "qsv", "amf", "rkmpp", "v4l2m2m",
        ]),
        hid: FeatureCapability::available(["otg", "ch9329", "ch9329_otg", "bluetooth", "none"]),
        atx: FeatureCapability::available(["gpio", "usb_relay", "serial", "wol", "none"]),
        msd: FeatureCapability::available(["configfs"]),
        otg: FeatureCapability::available(["configfs"]),
        audio: FeatureCapability::available(["alsa"]),
        rustdesk: FeatureCapability::available(["builtin"]),
        vnc: FeatureCapability::available(["builtin", "tight_jpeg", "h264"]),
        diagnostics: FeatureCapability::available(["linux"]),
        extensions: FeatureCapability::available(["linux"]),
        service_installation: FeatureCapability::available(["systemd"]),
    }
}

/// Checks whether the running platform is RK35xx (e.g. RK3588, RK3568, RK3566, RK3576) or newer.
/// Returns false for legacy Rockchip VPUs (e.g. RK3399, RK3328, RK3288) so that
/// software decode/conversion fallbacks can be used instead of unsupported direct input formats.
pub fn is_rk35xx_or_newer() -> bool {
    static CACHED: OnceLock<bool> = OnceLock::new();
    *CACHED.get_or_init(|| {
        // 1. Allow explicit override via environment variable
        if let Ok(val) = std::env::var("ONE_KVM_RKMPP_LEGACY_VPU") {
            if val == "1" || val.eq_ignore_ascii_case("true") {
                tracing::info!("[Rockchip] Force legacy VPU mode via ONE_KVM_RKMPP_LEGACY_VPU");
                return false;
            }
            if val == "0" || val.eq_ignore_ascii_case("false") {
                return true;
            }
        }

        // 2. Check /proc/device-tree/compatible
        if let Ok(compatible) = std::fs::read("/proc/device-tree/compatible") {
            let s = String::from_utf8_lossy(&compatible).to_lowercase();
            if s.contains("rk3399") || s.contains("rk3328") || s.contains("rk3288") {
                tracing::info!("[Rockchip] Detected legacy SoC (RK3399/RK3328/RK3288) via device-tree compatible");
                return false;
            }
            if s.contains("rk3588")
                || s.contains("rk3568")
                || s.contains("rk3566")
                || s.contains("rk3576")
                || s.contains("rk3528")
            {
                tracing::info!("[Rockchip] Detected modern RK35xx family SoC via device-tree compatible");
                return true;
            }
        }

        // 3. Check /sys/devices/soc0/soc_id
        if let Ok(soc_id) = std::fs::read_to_string("/sys/devices/soc0/soc_id") {
            let s = soc_id.to_lowercase();
            if s.contains("rk3399") || s.contains("rk3328") || s.contains("rk3288") {
                tracing::info!("[Rockchip] Detected legacy SoC via soc_id: {}", s.trim());
                return false;
            }
            if s.contains("rk3588")
                || s.contains("rk3568")
                || s.contains("rk3566")
                || s.contains("rk3576")
                || s.contains("rk3528")
            {
                tracing::info!("[Rockchip] Detected modern RK35xx family SoC via soc_id: {}", s.trim());
                return true;
            }
        }

        // 4. Check /proc/device-tree/model
        if let Ok(model) = std::fs::read("/proc/device-tree/model") {
            let s = String::from_utf8_lossy(&model).to_lowercase();
            if s.contains("rk3399") || s.contains("rk3328") || s.contains("rk3288") {
                tracing::info!("[Rockchip] Detected legacy SoC via device-tree model: {}", s.trim());
                return false;
            }
            if s.contains("rk3588")
                || s.contains("rk3568")
                || s.contains("rk3566")
                || s.contains("rk3576")
                || s.contains("rk3528")
            {
                return true;
            }
        }

        // 5. Check /proc/cpuinfo
        if let Ok(cpuinfo) = std::fs::read_to_string("/proc/cpuinfo") {
            let s = cpuinfo.to_lowercase();
            if s.contains("rk3399") || s.contains("rk3328") || s.contains("rk3288") {
                tracing::info!("[Rockchip] Detected legacy SoC via cpuinfo");
                return false;
            }
        }

        // 6. Default fallback: assume modern RK35xx so we never break original logic on unknown/newer hardware
        true
    })
}
