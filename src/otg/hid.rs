use std::path::{Path, PathBuf};
use tracing::debug;

use super::configfs::{
    create_dir, create_symlink, remove_dir, remove_file, write_bytes, write_file,
    write_file_if_exists,
};
use super::function::GadgetFunction;
use super::report_desc::{
    CONSUMER_CONTROL, KEYBOARD, KEYBOARD_WITH_LED, MOUSE_ABSOLUTE, MOUSE_RELATIVE,
};
use crate::error::Result;

#[derive(Debug, Clone)]
pub enum HidFunctionType {
    Keyboard,
    MouseRelative,
    MouseAbsolute,
    ConsumerControl,
}

impl HidFunctionType {
    pub fn protocol(&self) -> u8 {
        match self {
            HidFunctionType::Keyboard => 1,
            HidFunctionType::MouseRelative => 2,
            HidFunctionType::MouseAbsolute => 2,
            HidFunctionType::ConsumerControl => 0,
        }
    }

    pub fn subclass(&self) -> u8 {
        match self {
            HidFunctionType::Keyboard => 1,
            HidFunctionType::MouseRelative => 1,
            HidFunctionType::MouseAbsolute => 0,
            HidFunctionType::ConsumerControl => 0,
        }
    }

    pub fn report_length(&self, _keyboard_leds: bool) -> u8 {
        match self {
            HidFunctionType::Keyboard => 8,
            HidFunctionType::MouseRelative => 4,
            HidFunctionType::MouseAbsolute => 6,
            HidFunctionType::ConsumerControl => 2,
        }
    }

    pub fn report_desc(&self, keyboard_leds: bool) -> &'static [u8] {
        match self {
            HidFunctionType::Keyboard => {
                if keyboard_leds {
                    KEYBOARD_WITH_LED
                } else {
                    KEYBOARD
                }
            }
            HidFunctionType::MouseRelative => MOUSE_RELATIVE,
            HidFunctionType::MouseAbsolute => MOUSE_ABSOLUTE,
            HidFunctionType::ConsumerControl => CONSUMER_CONTROL,
        }
    }

    pub fn needs_out_endpoint(&self, keyboard_leds: bool) -> bool {
        matches!(self, HidFunctionType::Keyboard) && keyboard_leds
    }
}

#[derive(Debug, Clone)]
pub struct HidFunction {
    instance: u8,
    func_type: HidFunctionType,
    name: String,
    keyboard_leds: bool,
}

impl HidFunction {
    pub fn keyboard(instance: u8, keyboard_leds: bool) -> Self {
        Self {
            instance,
            func_type: HidFunctionType::Keyboard,
            name: format!("hid.usb{}", instance),
            keyboard_leds,
        }
    }

    pub fn mouse_relative(instance: u8) -> Self {
        Self {
            instance,
            func_type: HidFunctionType::MouseRelative,
            name: format!("hid.usb{}", instance),
            keyboard_leds: false,
        }
    }

    pub fn mouse_absolute(instance: u8) -> Self {
        Self {
            instance,
            func_type: HidFunctionType::MouseAbsolute,
            name: format!("hid.usb{}", instance),
            keyboard_leds: false,
        }
    }

    pub fn consumer_control(instance: u8) -> Self {
        Self {
            instance,
            func_type: HidFunctionType::ConsumerControl,
            name: format!("hid.usb{}", instance),
            keyboard_leds: false,
        }
    }

    fn function_path(&self, gadget_path: &Path) -> PathBuf {
        gadget_path.join("functions").join(self.name())
    }

    pub fn device_path(&self) -> PathBuf {
        PathBuf::from(format!("/dev/hidg{}", self.instance))
    }
}

impl GadgetFunction for HidFunction {
    fn name(&self) -> &str {
        &self.name
    }

    fn create(&self, gadget_path: &Path) -> Result<()> {
        let func_path = self.function_path(gadget_path);
        create_dir(&func_path)?;

        write_file(
            &func_path.join("protocol"),
            &self.func_type.protocol().to_string(),
        )?;
        write_file(
            &func_path.join("subclass"),
            &self.func_type.subclass().to_string(),
        )?;
        write_file(
            &func_path.join("report_length"),
            &self.func_type.report_length(self.keyboard_leds).to_string(),
        )?;

        write_bytes(
            &func_path.join("report_desc"),
            self.func_type.report_desc(self.keyboard_leds),
        )?;

        // Linux 6.2+ f_hid exposes `no_out_endpoint` so HID functions without
        // Output reports do not consume an Interrupt OUT endpoint on
        // endpoint-constrained UDCs (e.g. Allwinner sunxi_udc). Older kernels
        // simply do not expose this ConfigFS attribute.
        let no_out_endpoint = if self.func_type.needs_out_endpoint(self.keyboard_leds) {
            "0"
        } else {
            "1"
        };
        let _ = write_file_if_exists(&func_path.join("no_out_endpoint"), no_out_endpoint)?;

        // Supported by the PiKVM HID kernel patch. Older kernels simply do
        // not expose this ConfigFS attribute.
        let _ = write_file_if_exists(&func_path.join("wakeup_on_write"), "1")?;

        debug!(
            "Created HID function: {} at {}",
            self.name(),
            func_path.display()
        );
        Ok(())
    }

    fn link(&self, config_path: &Path, gadget_path: &Path) -> Result<()> {
        let func_path = self.function_path(gadget_path);
        let link_path = config_path.join(self.name());

        if !link_path.exists() {
            create_symlink(&func_path, &link_path)?;
            debug!("Linked HID function {} to config", self.name());
        }

        Ok(())
    }

    fn unlink(&self, config_path: &Path) -> Result<()> {
        let link_path = config_path.join(self.name());
        remove_file(&link_path)?;
        debug!("Unlinked HID function {}", self.name());
        Ok(())
    }

    fn cleanup(&self, gadget_path: &Path) -> Result<()> {
        let func_path = self.function_path(gadget_path);
        remove_dir(&func_path)?;
        debug!("Cleaned up HID function {}", self.name());
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_hid_function_types() {
        assert_eq!(HidFunctionType::Keyboard.report_length(false), 8);
        assert_eq!(HidFunctionType::Keyboard.report_length(true), 8);
        assert_eq!(HidFunctionType::MouseRelative.report_length(false), 4);
        assert_eq!(HidFunctionType::MouseAbsolute.report_length(false), 6);
    }

    #[test]
    fn test_hid_function_names() {
        let kb = HidFunction::keyboard(0, false);
        assert_eq!(kb.name(), "hid.usb0");
        assert_eq!(kb.device_path(), PathBuf::from("/dev/hidg0"));

        let mouse = HidFunction::mouse_relative(1);
        assert_eq!(mouse.name(), "hid.usb1");
    }

    #[test]
    fn test_hid_out_endpoint_requirements() {
        assert!(!HidFunctionType::Keyboard.needs_out_endpoint(false));
        assert!(HidFunctionType::Keyboard.needs_out_endpoint(true));
        assert!(!HidFunctionType::MouseRelative.needs_out_endpoint(true));
        assert!(!HidFunctionType::MouseAbsolute.needs_out_endpoint(true));
        assert!(!HidFunctionType::ConsumerControl.needs_out_endpoint(true));
    }

    #[test]
    fn test_hid_create_sets_no_out_endpoint_when_supported() {
        let cases = [
            (HidFunction::keyboard(0, false), "1"),
            (HidFunction::keyboard(1, true), "0"),
            (HidFunction::mouse_relative(2), "1"),
            (HidFunction::mouse_absolute(3), "1"),
            (HidFunction::consumer_control(4), "1"),
        ];

        for (func, expected) in cases {
            let temp = tempfile::tempdir().unwrap();
            let func_path = temp.path().join("functions").join(func.name());
            std::fs::create_dir_all(&func_path).unwrap();
            std::fs::write(func_path.join("no_out_endpoint"), b"0\n").unwrap();

            func.create(temp.path()).unwrap();

            assert_eq!(
                std::fs::read_to_string(func_path.join("no_out_endpoint"))
                    .unwrap()
                    .trim(),
                expected
            );
        }
    }

    #[test]
    fn test_hid_create_ignores_missing_no_out_endpoint() {
        let temp = tempfile::tempdir().unwrap();
        let func = HidFunction::mouse_absolute(0);

        func.create(temp.path()).unwrap();

        assert!(!temp
            .path()
            .join("functions")
            .join(func.name())
            .join("no_out_endpoint")
            .exists());
    }
}
