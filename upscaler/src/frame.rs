//! Packed RGB24 frame buffer shared by the decoder, upscalers and encoder.

#[derive(Clone)]
pub struct Frame {
    pub width: u32,
    pub height: u32,
    /// Row-major packed RGB, 3 bytes per pixel.
    pub data: Vec<u8>,
}

impl Frame {
    pub fn new(width: u32, height: u32) -> Self {
        Self { width, height, data: vec![0; width as usize * height as usize * 3] }
    }

}
