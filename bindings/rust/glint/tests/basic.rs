#[test]
fn encode_sine() {
    let mut enc = glint::Encoder::new(44100, 1, 128).unwrap();
    let samples: Vec<i16> = (0..44100)
        .map(|i| {
            (f64::sin(2.0 * std::f64::consts::PI * 440.0 * i as f64 / 44100.0) * 30000.0) as i16
        })
        .collect();

    let mut mp3 = Vec::new();
    for chunk in samples.chunks(enc.samples_per_frame()) {
        mp3.extend(enc.encode(chunk));
    }
    mp3.extend(enc.flush());

    assert!(mp3.len() > 1000);
    assert_eq!(mp3[0], 0xFF); // sync byte
}

#[test]
fn encode_pcm_convenience() {
    let samples: Vec<i16> = (0..44100)
        .map(|i| {
            (f64::sin(2.0 * std::f64::consts::PI * 440.0 * i as f64 / 44100.0) * 30000.0) as i16
        })
        .collect();

    let mp3 = glint::encode_pcm(&samples, 44100, 1, 128);
    assert!(mp3.len() > 1000);
    assert_eq!(mp3[0], 0xFF);
}

#[test]
fn encode_float_sine() {
    let mut enc = glint::Encoder::new(44100, 1, 128).unwrap();
    let samples: Vec<f32> = (0..44100)
        .map(|i| {
            (f64::sin(2.0 * std::f64::consts::PI * 440.0 * i as f64 / 44100.0) * 0.9) as f32
        })
        .collect();

    let mut mp3 = Vec::new();
    for chunk in samples.chunks(enc.samples_per_frame()) {
        mp3.extend(enc.encode_float(chunk));
    }
    mp3.extend(enc.flush());

    assert!(mp3.len() > 1000);
    assert_eq!(mp3[0], 0xFF);
}

#[test]
fn invalid_config() {
    let result = glint::Encoder::new(12345, 1, 128);
    assert!(result.is_err());
}
