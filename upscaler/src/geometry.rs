//! Final output frame size and the ffmpeg filter that produces it from the AI output.

use anyhow::{Result, bail};

/// How the picture is placed in a fixed `--size` frame whose shape differs from the source.
#[derive(Debug, Clone, Copy, PartialEq, Eq, clap::ValueEnum)]
pub enum Fit {
    /// Fit inside the frame and fill the rest with black bars (pillarbox/letterbox).
    Pad,
    /// Fill the frame and cut off what overflows.
    Crop,
    /// Fill the frame by distorting the picture.
    Stretch,
    /// Ignore the frame width: scale to its height and keep the picture's shape (no bars).
    Keep,
}

#[derive(Debug, Clone, PartialEq)]
pub struct OutputGeometry {
    /// ffmpeg `-vf` chain applied to the upscaled frames.
    pub filter: String,
    pub width: u32,
    pub height: u32,
}

/// Parses "1920x1080".
pub fn parse_size(s: &str) -> Result<(u32, u32)> {
    let Some((w, h)) = s.to_ascii_lowercase().split_once('x').map(|(w, h)| (w.trim().parse::<u32>(), h.trim().parse::<u32>())) else {
        bail!("size must look like 1920x1080, got {s:?}");
    };
    match (w, h) {
        (Ok(w), Ok(h)) if w >= 16 && h >= 16 => Ok((even(w), even(h))),
        _ => bail!("size must look like 1920x1080, got {s:?}"),
    }
}

fn even(v: u32) -> u32 {
    v + v % 2
}

fn even_round(v: f64) -> u32 {
    (((v / 2.0).round() as u32) * 2).max(2)
}

/// Works out the encoded frame size for AI output `ai_w`x`ai_h`.
///
/// `sar` is the source's sample (pixel) aspect ratio: DVDs often store 720x480 frames that
/// display at 4:3 or 16:9, so frames are first resampled to square pixels or the picture
/// would come out squashed. `size` is a target frame (`--size`); `height` alone (`--height`)
/// keeps the picture's shape.
pub fn output_geometry(ai_w: u32, ai_h: u32, sar: f64, size: Option<(u32, u32)>, height: Option<u32>, fit: Fit) -> OutputGeometry {
    let mut filters = Vec::new();

    // Square pixels first, so every later step works with the true picture shape.
    let mut disp_w = ai_w;
    if (sar - 1.0).abs() > 0.01 {
        disp_w = even_round(ai_w as f64 * sar);
        filters.push(format!("scale={disp_w}:{ai_h}:flags=lanczos"));
    }

    let keep_height = match (size, height) {
        (Some((_, h)), _) if fit == Fit::Keep => Some(h),
        (None, Some(h)) => Some(even(h)),
        _ => None,
    };

    let (width, height) = if let Some(h) = keep_height {
        let w = even_round(disp_w as f64 * h as f64 / ai_h as f64);
        filters.push(format!("scale={w}:{h}:flags=lanczos"));
        (w, h)
    } else if let Some((w, h)) = size {
        match fit {
            Fit::Pad => filters.push(format!(
                "scale={w}:{h}:force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=black"
            )),
            Fit::Crop => filters.push(format!(
                "scale={w}:{h}:force_original_aspect_ratio=increase:force_divisible_by=2:flags=lanczos,crop={w}:{h}"
            )),
            Fit::Stretch => filters.push(format!("scale={w}:{h}:flags=lanczos")),
            Fit::Keep => unreachable!("handled above"),
        }
        (w, h)
    } else {
        // yuv420p needs even dimensions; odd sizes can happen with x3 models.
        filters.push("pad=ceil(iw/2)*2:ceil(ih/2)*2".to_string());
        (even(disp_w), even(ai_h))
    };

    filters.push("setsar=1".to_string());
    OutputGeometry { filter: filters.join(","), width, height }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pal_4x3_into_1080p_frame_is_pillarboxed() {
        // 576x432 square-pixel source upscaled x4.
        let g = output_geometry(2304, 1728, 1.0, Some((1920, 1080)), None, Fit::Pad);
        assert_eq!((g.width, g.height), (1920, 1080));
        assert!(g.filter.contains("pad=1920:1080"));
    }

    #[test]
    fn every_standard_size_is_exact_for_every_source_shape() {
        let standards = [(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)];
        // (AI output w, h, SAR): 4:3 PAL, 3:2 NTSC square-pixel, anamorphic 16:9 NTSC, 2.39:1 scope.
        let sources = [(2304, 1728, 1.0), (2880, 1920, 1.0), (2880, 1920, 32.0 / 27.0), (3840, 1608, 1.0)];
        for (w, h) in standards {
            for (ai_w, ai_h, sar) in sources {
                for fit in [Fit::Pad, Fit::Crop, Fit::Stretch] {
                    let g = output_geometry(ai_w, ai_h, sar, Some((w, h)), None, fit);
                    assert_eq!((g.width, g.height), (w, h), "{ai_w}x{ai_h} sar {sar} {fit:?} into {w}x{h}");
                }
            }
        }
    }

    #[test]
    fn keep_shape_at_1080_lines() {
        let g = output_geometry(2304, 1728, 1.0, Some((1920, 1080)), None, Fit::Keep);
        assert_eq!((g.width, g.height), (1440, 1080));
    }

    #[test]
    fn legacy_height_keeps_shape() {
        let g = output_geometry(2304, 1728, 1.0, None, Some(1080), Fit::Pad);
        assert_eq!((g.width, g.height), (1440, 1080));
    }

    #[test]
    fn anamorphic_ntsc_widescreen_dvd() {
        // 720x480 stored, SAR 32:27 => displays 16:9. x4 => 2880x1920 stored.
        let g = output_geometry(2880, 1920, 32.0 / 27.0, None, None, Fit::Pad);
        assert_eq!((g.width, g.height), (3414, 1920));
        assert!(g.filter.starts_with("scale=3414:1920"));
        let g = output_geometry(2880, 1920, 32.0 / 27.0, None, Some(1080), Fit::Pad);
        assert_eq!((g.width, g.height), (1920, 1080));
    }

    #[test]
    fn model_scale_rounds_to_even() {
        let g = output_geometry(1083, 813, 1.0, None, None, Fit::Pad);
        assert_eq!((g.width, g.height), (1084, 814));
    }

    #[test]
    fn parses_sizes() {
        assert_eq!(parse_size("1920x1080").unwrap(), (1920, 1080));
        assert_eq!(parse_size("1281X721").unwrap(), (1282, 722));
        assert!(parse_size("1080p").is_err());
    }
}
