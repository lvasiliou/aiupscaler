//! Reads stream information from the input file with `ffprobe`.

use std::path::Path;
use std::process::Command;

use anyhow::{Context, Result, bail};
use serde::Deserialize;

#[derive(Debug, Clone)]
pub struct MediaInfo {
    pub width: u32,
    pub height: u32,
    /// Frame rate as an exact rational string ffmpeg understands, e.g. "30000/1001".
    pub fps: String,
    pub fps_value: f64,
    /// Estimated number of frames (for the progress bar); 0 when unknown.
    pub frame_count: u64,
    /// Duration in seconds; 0 when unknown.
    pub duration_secs: f64,
    /// Sample (pixel) aspect ratio, e.g. 32/27 for widescreen NTSC DVD; 1.0 for square pixels.
    pub sar: f64,
    pub has_audio: bool,
}

#[derive(Deserialize)]
struct ProbeOutput {
    streams: Vec<ProbeStream>,
    format: Option<ProbeFormat>,
}

#[derive(Deserialize)]
struct ProbeStream {
    codec_type: String,
    width: Option<u32>,
    height: Option<u32>,
    avg_frame_rate: Option<String>,
    r_frame_rate: Option<String>,
    nb_frames: Option<String>,
    duration: Option<String>,
    sample_aspect_ratio: Option<String>,
}

#[derive(Deserialize)]
struct ProbeFormat {
    duration: Option<String>,
}

fn parse_rational(s: &str) -> Option<f64> {
    let (num, den) = s.split_once('/').unwrap_or((s, "1"));
    let num: f64 = num.trim().parse().ok()?;
    let den: f64 = den.trim().parse().ok()?;
    (den != 0.0 && num > 0.0).then(|| num / den)
}

pub fn probe(ffprobe: &str, input: &Path) -> Result<MediaInfo> {
    let output = Command::new(ffprobe)
        .args(["-v", "error", "-print_format", "json", "-show_streams", "-show_format"])
        .arg(input)
        .output()
        .with_context(|| format!("failed to run `{ffprobe}` - is ffmpeg installed and on PATH?"))?;
    if !output.status.success() {
        bail!("ffprobe failed: {}", String::from_utf8_lossy(&output.stderr).trim());
    }

    let parsed: ProbeOutput = serde_json::from_slice(&output.stdout).context("could not parse ffprobe output")?;
    let video = parsed
        .streams
        .iter()
        .find(|s| s.codec_type == "video")
        .context("input has no video stream")?;
    let has_audio = parsed.streams.iter().any(|s| s.codec_type == "audio");

    let (width, height) = match (video.width, video.height) {
        (Some(w), Some(h)) if w > 0 && h > 0 => (w, h),
        _ => bail!("could not determine video dimensions"),
    };

    // Prefer the average frame rate; r_frame_rate can be a misleading "tick rate" for VFR content.
    let (fps, fps_value) = [&video.avg_frame_rate, &video.r_frame_rate]
        .into_iter()
        .flatten()
        .find_map(|s| parse_rational(s).map(|v| (s.clone(), v)))
        .context("could not determine frame rate")?;

    let duration_secs = video
        .duration
        .as_deref()
        .or(parsed.format.as_ref().and_then(|f| f.duration.as_deref()))
        .and_then(|d| d.parse::<f64>().ok())
        .unwrap_or(0.0);

    let frame_count = video
        .nb_frames
        .as_deref()
        .and_then(|n| n.parse::<u64>().ok())
        .filter(|&n| n > 0)
        .unwrap_or_else(|| (duration_secs * fps_value).round() as u64);

    // "0:1" and "N/A" mean unknown: assume square pixels.
    let sar = video
        .sample_aspect_ratio
        .as_deref()
        .map(|s| s.replace(':', "/"))
        .and_then(|s| parse_rational(&s))
        .unwrap_or(1.0);

    Ok(MediaInfo { width, height, fps, fps_value, frame_count, duration_secs, sar, has_audio })
}
