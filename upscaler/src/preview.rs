//! Writes source/output frame pairs as JPEG files for front ends to display.
//!
//! Files are written under a temporary name and renamed into place, so a reader never
//! sees a half-written image. Only the newest few pairs are kept on disk.

use std::collections::VecDeque;
use std::fs::File;
use std::io::BufWriter;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use image::ExtendedColorType;
use image::codecs::jpeg::JpegEncoder;

use crate::frame::Frame;

const JPEG_QUALITY: u8 = 92;
const KEEP_PAIRS: usize = 4;

pub struct PreviewWriter {
    dir: PathBuf,
    written: VecDeque<PathBuf>,
}

impl PreviewWriter {
    pub fn new(dir: &Path) -> Result<Self> {
        std::fs::create_dir_all(dir).with_context(|| format!("cannot create preview folder {}", dir.display()))?;
        Ok(Self { dir: dir.to_path_buf(), written: VecDeque::new() })
    }

    /// Saves `source` (and `output`, if given) for frame `id`; returns their paths.
    pub fn write(&mut self, id: u64, source: &Frame, output: Option<&Frame>) -> Result<(PathBuf, Option<PathBuf>)> {
        let source_path = self.save(&format!("src_{id}.jpg"), source)?;
        let output_path = output.map(|f| self.save(&format!("out_{id}.jpg"), f)).transpose()?;

        while self.written.len() > KEEP_PAIRS * 2 {
            if let Some(old) = self.written.pop_front() {
                let _ = std::fs::remove_file(old);
            }
        }
        Ok((source_path, output_path))
    }

    fn save(&mut self, name: &str, frame: &Frame) -> Result<PathBuf> {
        let path = self.dir.join(name);
        save_jpeg(&path, frame)?;
        self.written.push_back(path.clone());
        Ok(path)
    }
}

/// Writes `frame` as a JPEG via a temporary file and a rename, so readers never see a
/// partially written image.
pub fn save_jpeg(path: &Path, frame: &Frame) -> Result<()> {
    let mut temp = path.as_os_str().to_owned();
    temp.push(".part");
    let temp = PathBuf::from(temp);
    {
        let file = File::create(&temp).with_context(|| format!("cannot write {}", temp.display()))?;
        JpegEncoder::new_with_quality(BufWriter::new(file), JPEG_QUALITY)
            .encode(&frame.data, frame.width, frame.height, ExtendedColorType::Rgb8)
            .context("JPEG encoding failed")?;
    }
    std::fs::rename(&temp, path).with_context(|| format!("cannot write {}", path.display()))
}
