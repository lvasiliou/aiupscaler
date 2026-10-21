//! Audio improvements, applied with ffmpeg filters to the first audio track while the video is
//! encoded. Without any, the audio is copied bit-for-bit.

use clap::ValueEnum;

#[derive(Debug, Clone, Copy, PartialEq, Eq, ValueEnum)]
pub enum Enhancement {
    /// Clearer speech: separates the voices into the centre and turns them up.
    Dialogue,
    /// Evens out loud and quiet scenes (EBU R128 loudness normalisation).
    Loudness,
    /// Removes steady background noise, hiss and rumble.
    Denoise,
    /// Spreads stereo into 5.1 surround.
    Upmix,
}

/// What to do with the first audio track: its ffmpeg filter chain, and the codec and bitrate
/// to re-encode it with.
#[derive(Debug, Clone, PartialEq)]
pub struct AudioPlan {
    pub filter: String,
    pub codec: &'static str,
    pub bitrate: &'static str,
    /// Channels after the filters, e.g. 6 when stereo is upmixed.
    pub channels: u32,
}

/// Builds the filter chain for a track with `channels` channels in ffmpeg `layout` (e.g.
/// "stereo", "5.1(side)"). Returns `None` when there is nothing to do, so the track is copied.
pub fn plan(enhancements: &[Enhancement], channels: u32, layout: &str) -> Option<AudioPlan> {
    let has = |e| enhancements.contains(&e);
    let mut filters: Vec<String> = Vec::new();
    let mut channels = channels;
    let mut layout = layout.to_string();

    if has(Enhancement::Denoise) {
        // Rumble below 40 Hz, then adaptive spectral noise reduction that tracks the noise floor.
        filters.push("highpass=f=40".into());
        filters.push("afftdn=nr=12:nf=-30:tn=1".into());
    }
    if has(Enhancement::Upmix) && channels == 2 {
        filters.push("surround=chl_out=5.1".into());
        channels = 6;
        layout = "5.1".into();
    }
    if has(Enhancement::Dialogue) {
        if channels == 2 {
            // Extracts speech into a centre channel (3.0), boosts it, then folds back to stereo.
            // The fold-back is scaled by half (-6 dB, measured) so the mix keeps its overall
            // level and only the voices stand out.
            filters.push("dialoguenhance=original=1:enhance=2:voice=4".into());
            filters.push("pan=stereo|FL=0.5*FL+0.354*FC|FR=0.5*FR+0.354*FC".into());
        } else if channels >= 3 {
            // Surround mixes carry dialogue in the centre channel (the third one).
            let gains: Vec<String> = (0..channels).map(|c| if c == 2 { "c2=1.6*c2".to_string() } else { format!("c{c}=c{c}") }).collect();
            filters.push(format!("pan={layout}|{}", gains.join("|")));
        }
    }
    if has(Enhancement::Loudness) {
        // loudnorm works at 192 kHz internally; resample back to the usual 48 kHz.
        filters.push("loudnorm=I=-18:TP=-1.5:LRA=11".into());
        filters.push("aresample=48000".into());
    }
    if filters.is_empty() {
        return None;
    }
    // Boosted voices or upmixing can push peaks over full scale; keep them below it.
    filters.push("alimiter=limit=0.89:level=disabled".into());

    Some(AudioPlan {
        filter: filters.join(","),
        codec: "aac",
        bitrate: if channels > 2 { "448k" } else { "256k" },
        channels,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use Enhancement::*;

    #[test]
    fn nothing_to_do_copies() {
        assert_eq!(plan(&[], 2, "stereo"), None);
        // Upmix only applies to stereo, and dialogue needs at least two channels.
        assert_eq!(plan(&[Upmix], 6, "5.1"), None);
        assert_eq!(plan(&[Dialogue], 1, "mono"), None);
    }

    #[test]
    fn stereo_dialogue_folds_back_to_stereo() {
        let p = plan(&[Dialogue], 2, "stereo").unwrap();
        assert!(p.filter.starts_with("dialoguenhance"));
        assert!(p.filter.contains("pan=stereo"));
        assert_eq!((p.channels, p.bitrate), (2, "256k"));
    }

    #[test]
    fn surround_dialogue_boosts_centre_and_keeps_layout() {
        let p = plan(&[Dialogue], 6, "5.1(side)").unwrap();
        assert!(p.filter.starts_with("pan=5.1(side)|c0=c0|c1=c1|c2=1.6*c2|c3=c3|c4=c4|c5=c5,"));
        assert_eq!((p.channels, p.bitrate), (6, "448k"));
    }

    #[test]
    fn upmix_then_dialogue_boosts_new_centre() {
        let p = plan(&[Upmix, Dialogue, Loudness, Denoise], 2, "stereo").unwrap();
        let order: Vec<&str> = p.filter.split(',').map(|f| f.split('=').next().unwrap()).collect();
        assert_eq!(order, ["highpass", "afftdn", "surround", "pan", "loudnorm", "aresample", "alimiter"]);
        assert!(p.filter.contains("pan=5.1|"));
        assert_eq!(p.channels, 6);
    }
}
