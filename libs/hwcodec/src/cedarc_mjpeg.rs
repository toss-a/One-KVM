//! Allwinner CedarC zero-copy hardware MJPEG -> H.264/H.265 pipeline
//! (VE1 hardware JPEG decoder -> DMA-buf/IOMMU -> VE2 hardware H.264/H.265 encoder).
//! Loaded dynamically via dlopen() at runtime with no compile-time vendor library dependency.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::ptr::NonNull;

extern "C" {
    fn cedarc_mjpeg_h26x_supported() -> c_int;
    fn cedarc_mjpeg_h26x_new(
        codec: c_int,
        width: c_int,
        height: c_int,
        fps: c_int,
        kbps: c_int,
        gop: c_int,
    ) -> *mut c_void;
    fn cedarc_mjpeg_h26x_encode(
        pipeline: *mut c_void,
        jpeg_data: *const u8,
        jpeg_len: usize,
        pts_us: i64,
        force_idr: c_int,
        out_data: *mut *const u8,
        out_size: *mut usize,
        out_keyframe: *mut c_int,
    ) -> c_int;
    fn cedarc_mjpeg_h26x_reconfigure(pipeline: *mut c_void, kbps: c_int, gop: c_int) -> c_int;
    fn cedarc_mjpeg_h26x_free(pipeline: *mut c_void);
    fn cedarc_mjpeg_h26x_last_error() -> *const c_char;
}

pub fn is_supported() -> bool {
    unsafe { cedarc_mjpeg_h26x_supported() == 0 }
}

pub fn last_error_message() -> String {
    unsafe {
        let ptr = cedarc_mjpeg_h26x_last_error();
        if ptr.is_null() {
            String::new()
        } else {
            CStr::from_ptr(ptr).to_string_lossy().into_owned()
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum CedarcMjpegCodec {
    H264 = 0,
    H265 = 1,
}

#[derive(Debug, Clone, Copy)]
pub struct CedarcMjpegConfig {
    pub codec: CedarcMjpegCodec,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate_kbps: u32,
    pub gop: u32,
}

pub struct CedarcMjpegH26x {
    ctx: NonNull<c_void>,
}

unsafe impl Send for CedarcMjpegH26x {}

impl CedarcMjpegH26x {
    pub fn new(config: CedarcMjpegConfig) -> Result<Self, String> {
        let ptr = unsafe {
            cedarc_mjpeg_h26x_new(
                config.codec as c_int,
                config.width as c_int,
                config.height as c_int,
                config.fps as c_int,
                config.bitrate_kbps as c_int,
                config.gop.max(1) as c_int,
            )
        };
        Ok(Self {
            ctx: NonNull::new(ptr).ok_or_else(last_error_message)?,
        })
    }

    pub fn encode(
        &mut self,
        jpeg_data: &[u8],
        pts_ms: i64,
        force_idr: bool,
    ) -> Result<Option<(Vec<u8>, bool)>, String> {
        let mut out_data = std::ptr::null();
        let mut out_size = 0usize;
        let mut out_keyframe = 0 as c_int;
        let ret = unsafe {
            cedarc_mjpeg_h26x_encode(
                self.ctx.as_ptr(),
                jpeg_data.as_ptr(),
                jpeg_data.len(),
                pts_ms.saturating_mul(1000),
                force_idr as c_int,
                &mut out_data,
                &mut out_size,
                &mut out_keyframe,
            )
        };
        if ret != 0 {
            return Err(last_error_message());
        }
        if out_data.is_null() || out_size == 0 {
            return Ok(None);
        }
        let packet = unsafe { std::slice::from_raw_parts(out_data, out_size) }.to_vec();
        Ok(Some((packet, out_keyframe != 0)))
    }

    pub fn reconfigure(&mut self, kbps: u32, gop: u32) -> Result<(), String> {
        let ret = unsafe {
            cedarc_mjpeg_h26x_reconfigure(
                self.ctx.as_ptr(),
                kbps as c_int,
                gop.max(1) as c_int,
            )
        };
        if ret != 0 {
            return Err(last_error_message());
        }
        Ok(())
    }
}

impl Drop for CedarcMjpegH26x {
    fn drop(&mut self) {
        unsafe { cedarc_mjpeg_h26x_free(self.ctx.as_ptr()) };
    }
}
