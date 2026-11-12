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
/// Returns false for legacy Rockchip VPUs (e.g. RK3399, RK3328, RK3288) and unverified platforms so that
/// safe software decode/conversion fallbacks (MJPEG -> NV12 via libyuv, YUYV -> NV12) can be used
/// instead of unsupported direct NV16/YUYV hardware MJPEG pipelines.
pub fn is_rk35xx_or_newer() -> bool {
    static CACHED: OnceLock<bool> = OnceLock::new();
    *CACHED.get_or_init(|| {
        // 1. Check explicit environment variable overrides
        for env_name in &["ONE_KVM_RKMPP_LEGACY_VPU", "ONE_KVM_LEGACY_VPU"] {
            if let Ok(val) = std::env::var(env_name) {
                let v = val.trim().trim_matches('"').trim_matches('\'');
                if v == "1" || v.eq_ignore_ascii_case("true") {
                    tracing::info!("[Rockchip] Force legacy VPU mode via {}=1", env_name);
                    return false;
                }
                if v == "0" || v.eq_ignore_ascii_case("false") {
                    tracing::info!("[Rockchip] Force modern RK35xx mode via {}=0", env_name);
                    return true;
                }
            }
        }
        if let Ok(val) = std::env::var("ONE_KVM_RKMPP_HW_MJPEG") {
            let v = val.trim().trim_matches('"').trim_matches('\'');
            if v == "0" || v.eq_ignore_ascii_case("false") {
                tracing::info!("[Rockchip] HW MJPEG pipeline disabled via ONE_KVM_RKMPP_HW_MJPEG=0, using legacy pipeline");
                return false;
            }
        }
        for env_name in &["ONE_KVM_RKMPP_MODERN_VPU", "ONE_KVM_RKMPP_FORCE_MODERN"] {
            if let Ok(val) = std::env::var(env_name) {
                let v = val.trim().trim_matches('"').trim_matches('\'');
                if v == "1" || v.eq_ignore_ascii_case("true") {
                    tracing::info!("[Rockchip] Force modern RK35xx mode via {}=1", env_name);
                    return true;
                }
            }
        }

        let is_legacy_str = |s: &str| -> bool {
            s.contains("rk3399")
                || s.contains("rk3328")
                || s.contains("rk3288")
                || s.contains("rk3128")
                || s.contains("rk3066")
        };
        let is_modern_str = |s: &str| -> bool {
            s.contains("rk3588")
                || s.contains("rk3568")
                || s.contains("rk3566")
                || s.contains("rk3576")
                || s.contains("rk3528")
        };

        // 2. Check device-tree compatible strings
        let dt_compatible_paths = [
            "/proc/device-tree/compatible",
            "/sys/firmware/devicetree/base/compatible",
        ];
        for path in &dt_compatible_paths {
            if let Ok(bytes) = std::fs::read(path) {
                let s = String::from_utf8_lossy(&bytes).to_lowercase();
                if is_legacy_str(&s) {
                    tracing::info!("[Rockchip] Detected legacy SoC via {}: found legacy identifier", path);
                    return false;
                }
                if is_modern_str(&s) {
                    tracing::info!("[Rockchip] Detected modern RK35xx family SoC via {}", path);
                    return true;
                }
            }
        }

        // 3. Check sysfs soc bus attributes
        let soc_paths = [
            "/sys/bus/soc/devices/soc0/soc_id",
            "/sys/bus/soc/devices/soc0/family",
            "/sys/bus/soc/devices/soc0/machine",
            "/sys/devices/soc0/soc_id",
            "/sys/devices/soc0/family",
        ];
        for path in &soc_paths {
            if let Ok(content) = std::fs::read_to_string(path) {
                let s = content.to_lowercase();
                if is_legacy_str(&s) {
                    tracing::info!("[Rockchip] Detected legacy SoC via {}: {}", path, s.trim());
                    return false;
                }
                if is_modern_str(&s) {
                    tracing::info!("[Rockchip] Detected modern RK35xx family SoC via {}: {}", path, s.trim());
                    return true;
                }
            }
        }

        // 4. Check device-tree model
        let dt_model_paths = [
            "/proc/device-tree/model",
            "/sys/firmware/devicetree/base/model",
        ];
        for path in &dt_model_paths {
            if let Ok(bytes) = std::fs::read(path) {
                let s = String::from_utf8_lossy(&bytes).to_lowercase();
                if is_legacy_str(&s) {
                    tracing::info!("[Rockchip] Detected legacy SoC via {}: {}", path, s.trim());
                    return false;
                }
                if is_modern_str(&s) {
                    tracing::info!("[Rockchip] Detected modern RK35xx family SoC via {}: {}", path, s.trim());
                    return true;
                }
            }
        }

        // 5. Check /proc/cpuinfo
        if let Ok(cpuinfo) = std::fs::read_to_string("/proc/cpuinfo") {
            let s = cpuinfo.to_lowercase();
            if is_legacy_str(&s) {
                tracing::info!("[Rockchip] Detected legacy SoC via /proc/cpuinfo text match");
                return false;
            }
            if is_modern_str(&s) {
                tracing::info!("[Rockchip] Detected modern SoC via /proc/cpuinfo text match");
                return true;
            }
            // RK3399 big cores are Cortex-A72 (part 0xd08). In ARM64 Linux, "CPU part : 0xd08"
            // is uniquely Cortex-A72 (not used on RK35xx, which use A76 0xd0b and A55 0xd05).
            if s.contains("0xd08") {
                tracing::info!("[Rockchip] Detected Cortex-A72 CPU core (0xd08) in /proc/cpuinfo (RK3399 legacy SoC)");
                return false;
            }
        }

        // 6. Safe default fallback:
        // Default to false (safe legacy mode: CPU NV12 conversion + rkmpp hardware encode).
        // This guarantees RK3399, RK3328, RK3288, and unknown boards never crash with "Cannot allocate (-12) drm_fmt=0x3631564e".
        // RK35xx boards have standard compatible strings (rk3588, rk3568, etc.) and will be detected above.
        // If an unlisted RK35xx board is used, setting ONE_KVM_RKMPP_MODERN_VPU=1 will force modern mode.
        tracing::info!("[Rockchip] Modern RK35xx SoC not confirmed; defaulting to safe legacy mode (libyuv NV12 + rkmpp encode)");
        false
    })
}

