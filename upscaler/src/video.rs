//! ffmpeg-backed frame decoder and encoder.
//!
//! Frames travel as raw packed RGB24 through pipes, so any container/codec ffmpeg can read
//! works as input. The encoder re-opens the original file as a second input to carry its
//! audio tracks and metadata into the output untouched (or re-encoded, if requested).

use std::io::{ErrorKind, Read, Write};
use std::path::Path;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::thread::JoinHandle;

use anyhow::{Context, Result, bail};

use crate::frame::Frame;

/// Collects a child's stderr on a background thread so the pipe never fills up and
/// the text is available for error messages.
fn capture_stderr(child: &mut Child) -> JoinHandle<String> {
    let mut stderr = child.stderr.take().expect("stderr was piped");
    std::thread::spawn(move || {
        let mut buf = String::new();
        let _ = stderr.read_to_string(&mut buf);
        buf
    })
}

fn finish(mut child: Child, stderr: JoinHandle<String>, what: &str) -> Result<()> {
    let status = child.wait().with_context(|| format!("failed waiting for ffmpeg {what}"))?;
    let log = stderr.join().unwrap_or_default();
    if !status.success() {
        bail!("ffmpeg {what} exited with {status}:\n{}", log.trim());
    }
    Ok(())
}

pub struct Decoder {
    child: Child,
    stdout: ChildStdout,
    stderr: JoinHandle<String>,
    width: u32,
    height: u32,
}

impl Decoder {
    pub fn spawn(ffmpeg: &str, input: &Path, width: u32, height: u32, fps: &str, hwaccel: &str, start_secs: Option<f64>) -> Result<Self> {
        let mut cmd = Command::new(ffmpeg);
        cmd.args(["-hide_banner", "-loglevel", "error", "-nostdin"]);
        if let Some(start) = start_secs {
            // Input-side seek: fast keyframe seek, then exact decode to the timestamp.
            cmd.args(["-ss", &format!("{start:.3}")]);
        }
        if hwaccel != "none" {
            // GPU decode; frames are copied back to system memory for the RGB pipe.
            // With "auto", ffmpeg silently falls back to software if no decoder fits.
            cmd.args(["-hwaccel", hwaccel]);
        }
        let mut child = cmd
            .arg("-i")
            .arg(input)
            // Normalise to constant frame rate so the encoder's fixed -r keeps A/V sync.
            .args(["-map", "0:v:0", "-fps_mode", "cfr", "-r", fps])
            .args(["-f", "rawvideo", "-pix_fmt", "rgb24", "-"])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .with_context(|| format!("failed to start `{ffmpeg}` - is ffmpeg installed and on PATH?"))?;
        let stdout = child.stdout.take().expect("stdout was piped");
        let stderr = capture_stderr(&mut child);
        Ok(Self { child, stdout, stderr, width, height })
    }

    /// Reads the next frame, or `None` at end of stream.
    pub fn next_frame(&mut self) -> Result<Option<Frame>> {
        let mut frame = Frame::new(self.width, self.height);
        match self.stdout.read_exact(&mut frame.data) {
            Ok(()) => Ok(Some(frame)),
            Err(e) if e.kind() == ErrorKind::UnexpectedEof => Ok(None),
            Err(e) => Err(e).context("failed reading decoded frame"),
        }
    }

    pub fn finish(self) -> Result<()> {
        drop(self.stdout);
        finish(self.child, self.stderr, "decoder")
    }

    /// Stops decoding early (e.g. after --max-frames).
    pub fn kill(mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[derive(Debug, Clone)]
pub struct EncoderSettings {
    pub codec: String,
    pub crf: u32,
    pub preset: String,
    pub audio_codec: String,
    /// Improvements for the first audio track (re-encoded); other tracks use `audio_codec`.
    pub audio_plan: Option<crate::audio::AudioPlan>,
    /// Source subtitle tracks to keep, as (index among subtitle tracks, output codec); see
    /// `matroska_subtitles`. Empty for containers that don't carry them.
    pub subtitles: Vec<(usize, &'static str)>,
    /// Copy attachments (fonts for styled subtitles); Matroska only.
    pub attachments: bool,
    /// ffmpeg filter chain that turns AI output frames into the final frame size
    /// (see `geometry::output_geometry`).
    pub video_filter: String,
    /// Stop the output (including audio) after this many seconds, for previews.
    pub duration_limit: Option<f64>,
    pub extra_args: Vec<String>,
}

pub struct Encoder {
    child: Child,
    stdin: ChildStdin,
    stderr: JoinHandle<String>,
}

impl Encoder {
    #[allow(clippy::too_many_arguments)]
    pub fn spawn(
        ffmpeg: &str,
        source: &Path,
        output: &Path,
        width: u32,
        height: u32,
        fps: &str,
        include_audio: bool,
        settings: &EncoderSettings,
    ) -> Result<Self> {
        let mut cmd = Command::new(ffmpeg);
        cmd.args(["-hide_banner", "-loglevel", "error", "-y"])
            // Input 0: upscaled frames from our stdin.
            .args(["-f", "rawvideo", "-pix_fmt", "rgb24"])
            .args(["-s", &format!("{width}x{height}"), "-r", fps, "-i", "-"])
            // Input 1: the original file, for audio and metadata.
            .arg("-i")
            .arg(source)
            .args(["-map", "0:v:0", "-map_metadata", "1"]);

        if include_audio {
            cmd.args(["-map", "1:a?", "-c:a", &settings.audio_codec]);
            if let Some(plan) = &settings.audio_plan {
                cmd.args(["-filter:a:0", &plan.filter, "-c:a:0", plan.codec, "-b:a:0", plan.bitrate]);
            }
        }
        // Each track is mapped on its own: mapping all of them would also take tracks the
        // container rejects (e.g. MP4 chapter text tracks show up as data streams).
        for (out, (index, codec)) in settings.subtitles.iter().enumerate() {
            cmd.args(["-map", &format!("1:s:{index}"), &format!("-c:s:{out}"), codec]);
        }
        if settings.attachments {
            cmd.args(["-map", "1:t?", "-c:t", "copy"]);
        }

        let q = settings.crf.to_string();
        let codec = settings.codec.as_str();
        let pix_fmt = if codec.ends_with("_qsv") { "nv12" } else { "yuv420p" };
        cmd.args(["-vf", &settings.video_filter, "-pix_fmt", pix_fmt, "-c:v", codec]);
        // Map --crf onto each encoder family's constant-quality mode.
        match codec {
            "libx264" | "libx265" => {
                cmd.args(["-crf", &q, "-preset", &settings.preset]);
            }
            "libsvtav1" => {
                cmd.args(["-crf", &q]);
            }
            c if c.ends_with("_nvenc") => {
                cmd.args(["-preset", "p5", "-rc", "vbr", "-cq", &q, "-b:v", "0"]);
            }
            c if c.ends_with("_amf") => {
                cmd.args(["-quality", "quality", "-rc", "cqp", "-qp_i", &q, "-qp_p", &q]);
                if c != "av1_amf" {
                    cmd.args(["-qp_b", &q]);
                }
            }
            c if c.ends_with("_qsv") => {
                cmd.args(["-global_quality", &q]);
            }
            _ => {}
        }
        if codec == "libx265" || codec.starts_with("hevc_") {
            // Lets Apple players recognise HEVC in MP4/MOV.
            cmd.args(["-tag:v", "hvc1"]);
        }
        if matches!(output.extension().and_then(|e| e.to_str()), Some("mp4" | "mov" | "m4v")) {
            cmd.args(["-movflags", "+faststart"]);
        }
        if let Some(t) = settings.duration_limit {
            cmd.args(["-t", &format!("{t:.3}")]);
        }
        cmd.args(&settings.extra_args);
        cmd.arg(output);

        let mut child = cmd
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .spawn()
            .with_context(|| format!("failed to start `{ffmpeg}` encoder"))?;
        let stdin = child.stdin.take().expect("stdin was piped");
        let stderr = capture_stderr(&mut child);
        Ok(Self { child, stdin, stderr })
    }

    pub fn write_frame(&mut self, frame: &Frame) -> Result<()> {
        self.stdin.write_all(&frame.data).context("ffmpeg encoder closed its input (see error below)")
    }

    pub fn finish(self) -> Result<()> {
        drop(self.stdin);
        finish(self.child, self.stderr, "encoder")
    }

    /// Kills the encoder after a failure elsewhere, returning whatever it logged.
    pub fn abort(mut self) -> String {
        drop(self.stdin);
        let _ = self.child.kill();
        let _ = self.child.wait();
        self.stderr.join().unwrap_or_default()
    }
}

/// Hardware encoders to try for a codec family, in order of preference.
fn hw_encoder_candidates(family: &str) -> &'static [&'static str] {
    match family {
        "hevc" => &["hevc_nvenc", "hevc_amf", "hevc_qsv"],
        "av1" => &["av1_nvenc", "av1_amf", "av1_qsv"],
        _ => &["h264_nvenc", "h264_amf", "h264_qsv"],
    }
}

fn software_encoder(family: &str) -> &'static str {
    match family {
        "hevc" => "libx265",
        "av1" => "libsvtav1",
        _ => "libx264",
    }
}

/// True for outputs written as Matroska (.mkv), which can hold subtitle tracks and attachments.
pub fn is_matroska(output: &Path) -> bool {
    output.extension().and_then(|e| e.to_str()).is_some_and(|e| e.eq_ignore_ascii_case("mkv"))
}

/// Which source subtitle tracks (by codec, in order) a Matroska output keeps, and how:
/// (index among the subtitle tracks, ffmpeg codec). Formats Matroska stores are copied; MP4
/// text subtitles (mov_text) are converted to SRT; anything else (e.g. closed captions) is
/// left out rather than failing the whole encode.
pub fn matroska_subtitles(codecs: &[String]) -> Vec<(usize, &'static str)> {
    const COPY: &[&str] = &["subrip", "ass", "ssa", "webvtt", "dvd_subtitle", "hdmv_pgs_subtitle", "dvb_subtitle"];
    codecs
        .iter()
        .enumerate()
        .filter_map(|(i, codec)| match codec.as_str() {
            c if COPY.contains(&c) => Some((i, "copy")),
            "mov_text" | "text" => Some((i, "srt")),
            other => {
                eprintln!("Leaving out subtitle track {i}: {other} subtitles can't be stored in MKV");
                None
            }
        })
        .collect()
}

/// Resolves `auto`, `auto-hevc` and `auto-av1` to the first GPU encoder that can actually
/// open on this machine (ffmpeg lists encoders it was built with even when the GPU is
/// missing, so each one is test-encoded). Other codec names are returned unchanged.
pub fn resolve_codec(ffmpeg: &str, codec: &str) -> String {
    let family = match codec {
        "auto" | "auto-h264" => "h264",
        "auto-hevc" => "hevc",
        "auto-av1" => "av1",
        other => return other.to_string(),
    };
    if let Some(&candidate) = hw_encoder_candidates(family).iter().find(|c| encoder_works(ffmpeg, c)) {
        return candidate.to_string();
    }
    let fallback = software_encoder(family);
    eprintln!("No working GPU encoder for {family}; using {fallback}");
    fallback.to_string()
}

/// Test-encodes a fraction of a second of black frames with `encoder`.
fn encoder_works(ffmpeg: &str, encoder: &str) -> bool {
    let pix_fmt = if encoder.ends_with("_qsv") { "nv12" } else { "yuv420p" };
    Command::new(ffmpeg)
        .args(["-hide_banner", "-loglevel", "error", "-nostdin"])
        .args(["-f", "lavfi", "-i", "color=black:s=640x360:d=0.2"])
        .args(["-pix_fmt", pix_fmt, "-c:v", encoder, "-f", "null", "-"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

/// Which encoders work here, for `--capabilities`: for each GPU family (`auto`,
/// `auto-hevc`, `auto-av1`) the encoder `resolve_codec` would pick, or null; and whether
/// each software encoder is available. All candidates are tested in parallel.
pub fn probe_encoders(ffmpeg: &str) -> serde_json::Value {
    let families = [("auto", "h264"), ("auto-hevc", "hevc"), ("auto-av1", "av1")];
    let software = ["libx264", "libx265", "libsvtav1"];
    let candidates: Vec<&str> = families.iter().flat_map(|(_, f)| hw_encoder_candidates(f).iter().copied()).chain(software).collect();

    let working: Vec<bool> = std::thread::scope(|scope| {
        let handles: Vec<_> = candidates.iter().map(|c| scope.spawn(move || encoder_works(ffmpeg, c))).collect();
        handles.into_iter().map(|h| h.join().unwrap_or(false)).collect()
    });
    let works = |name: &str| candidates.iter().zip(&working).any(|(c, ok)| *c == name && *ok);

    let mut result = serde_json::Map::new();
    for (tag, family) in families {
        let pick = hw_encoder_candidates(family).iter().find(|c| works(c));
        result.insert(tag.to_string(), pick.map_or(serde_json::Value::Null, |c| serde_json::Value::from(*c)));
    }
    for name in software {
        result.insert(name.to_string(), serde_json::Value::from(works(name)));
    }
    serde_json::Value::Object(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matroska_output_is_detected_by_extension() {
        assert!(is_matroska(Path::new("a/b.mkv")));
        assert!(is_matroska(Path::new("B.MKV")));
        assert!(!is_matroska(Path::new("b.mp4")));
        assert!(!is_matroska(Path::new("mkv")));
    }

    #[test]
    fn matroska_subtitles_copy_convert_or_skip() {
        let codecs: Vec<String> = ["subrip", "mov_text", "eia_608", "hdmv_pgs_subtitle"].iter().map(|s| s.to_string()).collect();
        assert_eq!(matroska_subtitles(&codecs), vec![(0, "copy"), (1, "srt"), (3, "copy")]);
        assert!(matroska_subtitles(&[]).is_empty());
    }
}
