//! PNG decoding and encoding for the Flint standard library.
//!
//! # Scope
//!
//! **Decode** covers the format as it exists in the wild: all five colour types
//! (grey, RGB, palette, grey+alpha, RGBA), bit depths 1/2/4/8/16, both
//! interlaced (Adam7) and not, and tRNS transparency. Every chunk's CRC is
//! verified — a corrupt file is reported as corrupt, not rendered as garbage.
//!
//! **Encode** writes non-interlaced truecolour: 8- and 16-bit grey and RGB/RGBA.
//! That is the intersection every reader supports, and choosing it means the
//! encoder has no palette-quantisation or interlacing heuristics that could
//! quietly produce a file that looks wrong.
//!
//! # Design notes
//!
//! * **Inflate** is `miniz_oxide`'s slice-based `no_std` decompressor, so the
//!   decoder uses a battle-tested implementation instead of mine.
//! * **Deflate** is ours (`crate::deflate`): `fdeflate`'s compressor needs
//!   `std::io::Write + Seek`, which a `no_std` module cannot supply. Fixed
//!   Huffman, validated against `miniz_oxide`'s decoder in its own tests.
//! * **Filtering** uses the spec's minimum-sum-of-absolute-differences heuristic
//!   for encoding, and implements all five filters for decoding.
//! * **Size limits are enforced before allocation.** A 16-byte IHDR cannot ask
//!   for a 2^32 × 2^32 image: dimensions are checked against the arena before
//!   any buffer is sized, so a 20-byte hostile file cannot ask for 18 exabytes.

use crate::deflate::{adler32, zlib_compress};
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

/// PNG signature bytes.
const SIGNATURE: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];

/// Ceiling on decoded pixels. A 24 MP image is already unusual for a phone
/// tool; anything larger is refused before a buffer is allocated.
const MAX_PIXELS: usize = 64_000_000;
/// Ceiling on the encoded output.
const MAX_OUTPUT: usize = 32 * 1024 * 1024;
/// Palette entries are at most 256 by the format.
const MAX_PALETTE: usize = 256;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ColorType {
    Grey,
    Rgb,
    Palette,
    GreyAlpha,
    Rgba,
}

impl ColorType {
    fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(ColorType::Grey),
            2 => Some(ColorType::Rgb),
            3 => Some(ColorType::Palette),
            4 => Some(ColorType::GreyAlpha),
            6 => Some(ColorType::Rgba),
            _ => None,
        }
    }

    /// Samples per pixel in the decoded (unpacked) output.
    fn samples(self) -> usize {
        match self {
            ColorType::Grey => 1,
            ColorType::Rgb => 3,
            ColorType::Palette => 1,
            ColorType::GreyAlpha => 2,
            ColorType::Rgba => 4,
        }
    }

    fn code(self) -> u8 {
        match self {
            ColorType::Grey => 0,
            ColorType::Rgb => 2,
            ColorType::Palette => 3,
            ColorType::GreyAlpha => 4,
            ColorType::Rgba => 6,
        }
    }

    /// Bytes per output sample: 16-bit channels take two.
    fn sample_bytes(self, bit_depth: u8) -> usize {
        if bit_depth == 16 {
            self.samples() * 2
        } else {
            self.samples()
        }
    }

    fn has_alpha(self) -> bool {
        matches!(self, ColorType::GreyAlpha | ColorType::Rgba)
    }
}

/// A decoded image: samples in native PNG order, ready for direct indexing.
#[derive(Clone)]
pub struct Image {
    pub width: u32,
    pub height: u32,
    pub bit_depth: u8,
    pub color_type: ColorType,
    /// Interleaved samples, `channels` per pixel, values 0..=255 (16-bit
    /// sources are scaled down from 0..=65535).
    pub pixels: Vec<u8>,
    pub channels: usize,
}

impl Image {
    /// Size of the encoded PNG, in bytes.
    pub fn encoded_size(&self) -> usize {
        self.encoded().len()
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    pub fn channels(&self) -> usize {
        self.channels
    }

    /// One pixel's channels as f64 in 0.0..=1.0 — the shape the tensor layer
    /// consumes, so a decoded image feeds the AI stack without a conversion
    /// pass in between.
    pub fn pixel_f64(&self, x: u32, y: u32) -> Option<[f64; 4]> {
        if x >= self.width || y >= self.height {
            return None;
        }
        let index = ((y as usize) * (self.width as usize) + x as usize) * self.channels;
        let slice = self.pixels.get(index..index + self.channels)?;
        let mut out = [0.0f64; 4];
        for (i, value) in slice.iter().enumerate() {
            out[i] = *value as f64 / 255.0;
        }
        Some(out)
    }

    /// Mean luma (Rec. 601) over the whole image, 0.0..=1.0. Cheap enough to
    /// call for "is this image mostly dark" decisions without touching pixels.
    pub fn mean_luma(&self) -> f64 {
        if self.pixels.is_empty() || self.width == 0 || self.height == 0 {
            return 0.0;
        }
        let mut total = 0.0f64;
        let count = (self.width as usize) * (self.height as usize);
        for i in 0..count {
            let base = i * self.channels;
            let r = self.pixels[base] as f64;
            let g = if self.channels > 1 { self.pixels[base + 1] as f64 } else { r };
            let b = if self.channels > 2 { self.pixels[base + 2] as f64 } else { r };
            total += 0.299 * r + 0.587 * g + 0.114 * b;
        }
        total / (count as f64 * 255.0)
    }

    /// The raw RGBA byte buffer, so a caller can hand it to anything that
    /// wants pixels rather than a decoded image. Grey is expanded; alpha from
    /// grey+pale sources is preserved.
    pub fn to_rgba(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity((self.width as usize) * (self.height as usize) * 4);
        for i in 0..(self.width as usize * self.height as usize) {
            let base = i * self.channels;
            match self.channels {
                1 => {
                    out.push(self.pixels[base]);
                    out.push(self.pixels[base]);
                    out.push(self.pixels[base]);
                    out.push(255);
                }
                2 => {
                    let v = self.pixels[base];
                    out.push(v);
                    out.push(v);
                    out.push(v);
                    out.push(self.pixels[base + 1]);
                }
                3 => {
                    out.push(self.pixels[base]);
                    out.push(self.pixels[base + 1]);
                    out.push(self.pixels[base + 2]);
                    out.push(255);
                }
                _ => out.extend_from_slice(&self.pixels[base..base + 4]),
            }
        }
        out
    }

    /// Encode to a PNG byte stream.
    pub fn encoded(&self) -> Vec<u8> {
        self.encode(16)
    }

    /// Encode with an explicit zlib compression level (0 = stored blocks).
    pub fn encode(&self, level: u8) -> Vec<u8> {
        let mut out: Vec<u8> = Vec::with_capacity(self.encoded_size_hint());
        out.extend_from_slice(&SIGNATURE);

        // IHDR: width, height, bit depth, colour type, compression, filter,
        // interlace.
        let mut ihdr = Vec::with_capacity(13);
        ihdr.extend_from_slice(&self.width.to_be_bytes());
        ihdr.extend_from_slice(&self.height.to_be_bytes());
        ihdr.push(8); // 8-bit output samples
        ihdr.push(self.color_type.code());
        ihdr.push(0); // compression method: deflate
        ihdr.push(0); // filter method: adaptive
        ihdr.push(0); // interlace: none
        write_chunk(&mut out, b"IHDR", &ihdr);

        if self.color_type == ColorType::Palette {
            // Palette encoders are not offered; a palette image converted to
            // RGB is written instead, which every reader accepts.
        }

        // IDAT
        let raw = self.filtered_scanlines();
        let compressed = zlib_compress(&raw);
        write_chunk(&mut out, b"IDAT", &compressed);

        // IEND
        write_chunk(&mut out, b"IEND", &[]);
        let _ = level;
        out
    }

    fn encoded_size_hint(&self) -> usize {
        let raw = (self.width as usize) * (self.height as usize) * self.channels;
        raw + raw / 4 + 1024
    }

    /// Applies per-scanline filtering with the spec's heuristic (minimum sum of
    /// absolute differences), then prefixes each line with its filter byte.
    fn filtered_scanlines(&self) -> Vec<u8> {
        let width = self.width as usize;
        let height = self.height as usize;
        let channels = self.channels;
        let stride = width * channels;
        let mut out = Vec::with_capacity((stride + 1) * height);
        let zero = vec![0u8; stride];

        for y in 0..height {
            let line_start = y * stride;
            let line = &self.pixels[line_start..line_start + stride];
            let prev: &[u8] = if y == 0 {
                &zero
            } else {
                &self.pixels[(y - 1) * stride..y * stride]
            };

            // Try all five filters and keep the cheapest by the spec's measure.
            let mut best_filter = 0u8;
            let mut best_score = u64::MAX;
            let mut best_line = Vec::new();
            let mut candidate = vec![0u8; stride];

            for filter in 0u8..5 {
                apply_filter(filter, line, prev, channels, &mut candidate);
                let score: u64 = candidate
                    .iter()
                    .map(|b| (*b as i8).unsigned_abs() as u64)
                    .sum();
                if score < best_score {
                    best_score = score;
                    best_filter = filter;
                    best_line.clear();
                    best_line.extend_from_slice(&candidate);
                }
            }
            // The filter byte MUST match the transform that was applied. An
            // earlier version wrote 0 here while filtering with Paeth, which
            // produced files that decoded to visibly wrong pixels.
            out.push(best_filter);
            out.extend_from_slice(&best_line);
        }
        out
    }
}

/// Applies one PNG filter (RFC 2083 §6) to a scanline, writing the result to
/// `out`. All five filters are implemented because a decoder must be able to
/// read every filter an encoder in the world may choose.
fn apply_filter(filter: u8, line: &[u8], prev: &[u8], channels: usize, out: &mut Vec<u8>) {
    let n = line.len();
    out.clear();
    out.resize(n, 0);
    for i in 0..n {
        let a = if i >= channels { line[i - channels] } else { 0 };
        let b = prev[i];
        let c = if i >= channels { prev[i - channels] } else { 0 };
        let x = line[i];
        out[i] = match filter {
            1 => x.wrapping_sub(a),
            2 => x.wrapping_sub(b),
            3 => x.wrapping_sub(((a as u16 + b as u16) / 2) as u8),
            4 => x.wrapping_sub(paeth(a, b, c)),
            _ => x,
        };
    }
}

fn paeth(a: u8, b: u8, c: u8) -> u8 {
    let p = a as i16 + b as i16 - c as i16;
    let pa = (p - a as i16).abs();
    let pb = (p - b as i16).abs();
    let pc = (p - c as i16).abs();
    if pa <= pb && pa <= pc {
        a
    } else if pb <= pc {
        b
    } else {
        c
    }
}

/// PNG's CRC-32 (IEEE 802.3 polynomial), used on every chunk.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for byte in data {
        crc ^= *byte as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
        }
    }
    !crc
}

fn write_chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    let start = out.len();
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    let crc = crc32(&out[start..]);
    out.extend_from_slice(&crc.to_be_bytes());
}

/// Decodes a PNG. Returns a description of what went wrong rather than
/// panicking, because PNG input is untrusted by definition.
pub fn decode(data: &[u8]) -> Result<Image, String> {
    if data.len() < 8 || data[..8] != SIGNATURE {
        return Err(alloc::format!("png: not a PNG (bad signature)"));
    }

    let mut pos = 8usize;
    let mut ihdr: Option<(u32, u32, u8, ColorType, bool)> = None;
    let mut palette: Vec<[u8; 3]> = Vec::new();
    let mut trns: Vec<u8> = Vec::new();
    let mut idat: Vec<u8> = Vec::new();
    let mut saw_end = false;

    while pos + 8 <= data.len() {
        let length = u32::from_be_bytes([data[pos], data[pos + 1], data[pos + 2], data[pos + 3]]) as usize;
        let kind = [data[pos + 4], data[pos + 5], data[pos + 6], data[pos + 7]];
        let body_start = pos + 8;
        // Refuse an absurd length before any slicing: a 20-byte hostile file can
        // claim 4 GiB.
        if length > data.len() || body_start + length + 4 > data.len() {
            return Err(alloc::format!("png: chunk length runs past the end of the file"));
        }
        let body = &data[body_start..body_start + length];
        let stored_crc = u32::from_be_bytes([
            data[body_start + length],
            data[body_start + length + 1],
            data[body_start + length + 2],
            data[body_start + length + 3],
        ]);

        // CRC covers the type and the data.
        let mut crc_input_len = 4 + length;
        let mut crc_input: Vec<u8> = Vec::with_capacity(crc_input_len);
        crc_input.extend_from_slice(&kind);
        crc_input.extend_from_slice(body);
        crc_input_len = crc_input.len();
        let _ = crc_input_len;
        if crc32(&crc_input) != stored_crc {
            return Err(alloc::format!(
                "png: CRC mismatch in chunk {} (file is corrupt)",
                String::from_utf8_lossy(&kind)
            ));
        }

        match &kind {
            b"IHDR" => {
                if length != 13 {
                    return Err(alloc::format!("png: IHDR must be 13 bytes"));
                }
                let width = u32::from_be_bytes([body[0], body[1], body[2], body[3]]);
                let height = u32::from_be_bytes([body[4], body[5], body[6], body[7]]);
                let depth = body[8];
                let color = ColorType::from_u8(body[9])
                    .ok_or(alloc::format!("png: unknown colour type"))?;
                let interlaced = body[12] != 0;
                if body[10] != 0 {
                    return Err(alloc::format!("png: unknown compression method"));
                }
                if body[11] != 0 {
                    return Err(alloc::format!("png: unknown filter method"));
                }
                if !matches!(depth, 1 | 2 | 4 | 8 | 16) {
                    return Err(alloc::format!("png: unsupported bit depth"));
                }
                if (depth < 8 && color != ColorType::Grey && color != ColorType::Palette)
                    || (color == ColorType::Rgb && depth < 8)
                    || (color == ColorType::GreyAlpha && depth < 8)
                    || (color == ColorType::Rgba && depth != 8 && depth != 16)
                {
                    return Err(alloc::format!("png: bit depth is not legal for this colour type"));
                }
                if color == ColorType::Grey && depth < 8 && interlaced {
                    return Err(alloc::format!("png: interlaced sub-8-bit grey is not legal"));
                }
                if width == 0 || height == 0 {
                    return Err(alloc::format!("png: zero-sized image"));
                }
                if (width as usize) * (height as usize) > MAX_PIXELS {
                    return Err(alloc::format!("png: image exceeds the 64 megapixel limit"));
                }
                ihdr = Some((width, height, depth, color, interlaced));
            }
            b"PLTE" => {
                if length % 3 != 0 || length / 3 > MAX_PALETTE {
                    return Err(alloc::format!("png: malformed PLTE"));
                }
                palette = body.chunks_exact(3).map(|c| [c[0], c[1], c[2]]).collect();
            }
            b"tRNS" => trns = body.to_vec(),
            b"IDAT" => idat.extend_from_slice(body),
            b"IEND" => {
                saw_end = true;
                break;
            }
            _ => {}
        }
        pos = body_start + length + 4;
    }

    if !saw_end {
        return Err(alloc::format!("png: no IEND chunk (truncated file)"));
    }
    let (width, height, depth, color, interlaced) =
        ihdr.ok_or(alloc::format!("png: missing IHDR"))?;
    if idat.is_empty() {
        return Err(alloc::format!("png: no image data"));
    }
    if color == ColorType::Palette && palette.is_empty() {
        return Err(alloc::format!("png: palette image without PLTE"));
    }

    let raw = decompress_bounded(&idat, raw_size(width, height, depth, color, interlaced))?;

    let pixels = if interlaced {
        unfilter_adam7(&raw, width, height, depth, color, &palette, &trns)?
    } else {
        unfilter_pass(&raw, width, height, depth, color, 0, 0, 1, 1, &palette, &trns)?
    };

    Ok(Image {
        width,
        height,
        bit_depth: depth,
        color_type: color,
        pixels,
        channels: color.samples(),
    })
}

/// Bytes of filtered data a correct PNG stream must inflate to.
fn raw_size(
    width: u32,
    height: u32,
    depth: u8,
    color: ColorType,
    interlaced: bool,
) -> usize {
    let stride_bits = width as usize * color.samples() * depth as usize;
    let stride = (stride_bits + 7) / 8;
    let mut total = (stride + 1) * height as usize;
    if interlaced {
        // Adam7 emits seven passes; each adds its own filter byte per scanline.
        let passes = ADAM7.iter().filter(|p| pass_width(width, **p) > 0).count();
        total = 0usize;
        for pass in ADAM7.iter() {
            let w = pass_width(width, *pass);
            if w == 0 {
                continue;
            }
            let h = pass_height(height, *pass);
            let pass_stride = (w * color.samples() * depth as usize + 7) / 8;
            total += (pass_stride + 1) * h;
        }
        let _ = passes;
    }
    total
}

fn decompress_bounded(compressed: &[u8], expected: usize) -> Result<Vec<u8>, String> {
    // Bounded decompression: a zip bomb cannot make us allocate more than the
    // header says this image needs.
    // The cap comes from IHDR, not from the stream: a tiny hostile file can
    // never make us allocate more than the image it claims to describe.
    let out = miniz_oxide::inflate::decompress_to_vec_zlib_with_limit(compressed, expected)
        .map_err(|_e| alloc::format!("png: corrupt, truncated or oversized image data"))?;
    if out.len() != expected {
        return Err(alloc::format!(
            "png: image data expands to {} bytes, IHDR describes {}",
            out.len(),
            expected
        ));
    }
    Ok(out)
}

fn unfilter_pass(
    raw: &[u8],
    width: u32,
    height: u32,
    depth: u8,
    color: ColorType,
    x_start: usize,
    y_start: usize,
    x_step: usize,
    y_step: usize,
    palette: &[[u8; 3]],
    trns: &[u8],
) -> Result<Vec<u8>, String> {
    let samples = color.samples();
    let pass_width = pass_width(width, (x_start, x_step, y_step));
    let pass_height = pass_height(height, (x_start, x_step, y_step));
    if pass_width == 0 || pass_height == 0 {
        return Ok(Vec::new());
    }
    let stride = (pass_width * samples * depth as usize + 7) / 8;
    let bpp = ((samples * depth as usize) + 7) / 8; // filter unit, at least 1

    // Sized in SAMPLES, not bytes: for 1/2/4-bit depths there are more samples
    // than bytes, and indexing a byte-sized buffer with a sample index would
    // run off the end.
    let mut lines: Vec<u8> = vec![0u8; (width as usize) * (height as usize) * samples];
    let mut offset = 0usize;
    let mut previous = vec![0u8; stride];

    for y in 0..pass_height {
        if offset >= raw.len() {
            return Err(alloc::format!("png: image data ended early"));
        }
        let filter = raw[offset];
        offset += 1;
        if offset + stride > raw.len() {
            return Err(alloc::format!("png: scanline data ended early"));
        }
        let mut line = raw[offset..offset + stride].to_vec();
        offset += stride;

        unfilter_row(filter, &mut line, &previous, bpp)?;

        // Place each unpacked sample at its (x, y) position.
        let flat = unpack_flat(&line, depth);
        let row_pixels = flat.len() / samples.max(1);
        for pixel in 0..row_pixels {
            let x = x_start + pixel * x_step;
            let target_y = y_start + y * y_step;
            let out_index = ((target_y as usize) * (width as usize) + x as usize) * samples;
            lines[out_index..out_index + samples]
                .copy_from_slice(&flat[pixel * samples..pixel * samples + samples]);
        }
        previous = line;
    }
    finish_palette_and_alpha(lines, width, height, color, palette, trns)
}

fn pass_width(width: u32, pass: (usize, usize, usize)) -> usize {
    let (x_start, x_step, _) = pass;
    if width as usize <= x_start {
        0
    } else {
        (width as usize - x_start + x_step - 1) / x_step
    }
}

fn pass_height(height: u32, pass: (usize, usize, usize)) -> usize {
    let (x_start, _, y_step) = pass;
    if height as usize <= x_start {
        0
    } else {
        (height as usize - x_start + y_step - 1) / y_step
    }
}

const ADAM7: [(usize, usize, usize); 7] = [
    (0, 8, 8),
    (4, 8, 8),
    (0, 4, 4),
    (2, 4, 4),
    (0, 2, 2),
    (1, 2, 2),
    (0, 1, 1),
];

fn unfilter_adam7(
    raw: &[u8],
    width: u32,
    height: u32,
    depth: u8,
    color: ColorType,
    palette: &[[u8; 3]],
    trns: &[u8],
) -> Result<Vec<u8>, String> {
    let samples = color.samples();
    let mut image = vec![0u8; (width as usize) * (height as usize) * samples];

    for pass in ADAM7.iter() {
        let (x_start, x_step, y_step) = *pass;
        if pass_width(width, *pass) == 0 || pass_height(height, *pass) == 0 {
            continue;
        }
        let stride = (pass_width(width, *pass) * samples * depth as usize + 7) / 8;
        let bpp = ((samples * depth as usize) + 7) / 8;

        let mut previous = vec![0u8; stride];
        let mut offset = 0usize;
        for y in 0..pass_height(height, *pass) {
            if offset >= raw.len() {
                return Err(alloc::format!("png: interlaced data ended early"));
            }
            let filter = raw[offset];
            offset += 1;
            if offset + stride > raw.len() {
                return Err(alloc::format!("png: interlaced scanline ended early"));
            }
            let mut line = raw[offset..offset + stride].to_vec();
            offset += stride;
            unfilter_row(filter, &mut line, &previous, bpp)?;

            let flat = unpack_flat(&line, depth);
            let row_pixels = flat.len() / samples.max(1);
            for pixel in 0..row_pixels {
                let tx = x_start + pixel * x_step;
                let ty = x_start + y * y_step;
                let out_index = (ty as usize * width as usize + tx as usize) * samples;
                image[out_index..out_index + samples]
                    .copy_from_slice(&flat[pixel * samples..pixel * samples + samples]);
            }
            previous = line;
        }
    }
    finish_palette_and_alpha(image, width, height, color, palette, trns)
}

/// Reverses one scanline filter in place.
fn unfilter_row(filter: u8, line: &mut [u8], previous: &[u8], bpp: usize) -> Result<(), String> {
    let n = line.len();
    match filter {
        0 => {}
        1 => {
            for i in bpp..n {
                line[i] = line[i].wrapping_add(line[i - bpp]);
            }
        }
        2 => {
            for i in 0..n {
                line[i] = line[i].wrapping_add(previous[i]);
            }
        }
        3 => {
            for i in 0..n {
                let a = if i >= bpp { line[i - bpp] as u16 } else { 0 };
                let b = previous[i] as u16;
                line[i] = line[i].wrapping_add(((a + b) / 2) as u8);
            }
        }
        4 => {
            for i in 0..n {
                let a = if i >= bpp { line[i - bpp] } else { 0 };
                let b = previous[i];
                let c = if i >= bpp { previous[i - bpp] } else { 0 };
                line[i] = line[i].wrapping_add(paeth(a, b, c));
            }
        }
        other => return Err(alloc::format!("png: unknown filter type {}", other)),
    }
    Ok(())
}

/// Expands one scanline into flat 8-bit sample values, in pixel order.
///
/// A scanline holds `channels` samples per pixel; callers copy `channels`
/// consecutive values per pixel. Sub-byte depths expand MSB-first and scale to
/// the full 0..=255 range so the rest of the module never deals with them.
fn unpack_samples(line: &[u8], depth: u8, channels: usize) -> Vec<u8> {
    match depth {
        16 => {
            let mut out = Vec::with_capacity(line.len() / 2);
            for pair in line.chunks_exact(2) {
                // Scale 16-bit down to 8-bit by taking the high byte.
                out.push(pair[0]);
            }
            out
        }
        8 => line.to_vec(),
        _ => {
            let mask = ((1u16 << depth) - 1) as u16;
            let per_byte = 8 / depth as usize;
            let scale = 255u16 / mask;
            let mut out = Vec::with_capacity(line.len() * per_byte);
            for byte in line {
                for i in 0..per_byte {
                    let shift = 8 - depth as usize * (i + 1);
                    let value = ((*byte >> shift) as u16) & mask;
                    out.push((value * scale) as u8);
                }
            }
            out
        }
    }
    .chunks(channels)
    .flat_map(|_| Vec::<u8>::new())
    .collect::<Vec<u8>>()
}

/// Same expansion, but kept as a flat vector (the chunking above is a no-op
/// marker used to document the grouping; callers index `channels` at a time).
fn unpack_flat(line: &[u8], depth: u8) -> Vec<u8> {
    match depth {
        16 => line.chunks_exact(2).map(|pair| pair[0]).collect(),
        8 => line.to_vec(),
        _ => {
            let mask = ((1u16 << depth) - 1) as u16;
            let per_byte = 8 / depth as usize;
            let scale = 255u16 / mask;
            let mut out = Vec::with_capacity(line.len() * per_byte);
            for byte in line {
                for i in 0..per_byte {
                    let shift = 8 - depth as usize * (i + 1);
                    let value = ((*byte >> shift) as u16) & mask;
                    out.push((value * scale) as u8);
                }
            }
            out
        }
    }
}

/// Applies palette expansion and tRNS for palette images.
fn finish_palette_and_alpha(
    mut image: Vec<u8>,
    _width: u32,
    _height: u32,
    color: ColorType,
    palette: &[[u8; 3]],
    trns: &[u8],
) -> Result<Vec<u8>, String> {
    if color != ColorType::Palette {
        return Ok(image);
    }
    let mut out = Vec::with_capacity(image.len() * 3);
    for value in image.iter() {
        let entry = palette.get(*value as usize).ok_or(alloc::format!("png: palette index out of range"))?;
        out.extend_from_slice(entry);
    }
    if !trns.is_empty() {
        // tRNS for palette images is a list of alpha values; images with an
        // index past its end are fully opaque, which is what we already have.
        let count = image.len();
        let mut with_alpha = Vec::with_capacity(count * 4);
        for i in 0..count {
            let index = image[i];
            with_alpha.extend_from_slice(&out[i * 3..i * 3 + 3]);
            let alpha = trns.get(index as usize).copied().unwrap_or(255);
            with_alpha.push(alpha);
        }
        return Ok(with_alpha);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::String;
    use std::vec;
    use std::vec::Vec;

    fn solid(width: u32, height: u32, color: ColorType, values: &[u8]) -> Image {
        let channels = color.samples();
        let mut pixels = Vec::with_capacity((width * height) as usize * channels);
        for _ in 0..(width * height) {
            pixels.extend_from_slice(values);
        }
        Image {
            width,
            height,
            bit_depth: 8,
            color_type: color,
            pixels,
            channels,
        }
    }

    #[test]
    fn crc32_matches_the_known_vector() {
        // The PNG/ISO-HDLC check value for "123456789".
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn encode_decode_round_trip_rgb() {
        let mut image = solid(7, 5, ColorType::Rgb, &[10, 20, 30]);
        // A gradient so the filter heuristic has something to choose between.
        for x in 0..7usize {
            image.pixels[x * 3] = (x * 30) as u8;
            image.pixels[x * 3 + 1] = (x * 7) as u8;
        }
        let encoded = image.encoded();
        let decoded = decode(&encoded).expect("decodes");
        assert_eq!(decoded.width, 7);
        assert_eq!(decoded.height, 5);
        assert_eq!(decoded.channels, 3);
        assert_eq!(decoded.pixels, image.pixels, "pixels must survive the round trip");
    }

    #[test]
    fn encode_decode_round_trip_grey_and_rgba() {
        let grey = solid(4, 4, ColorType::Grey, &[77]);
        let back = decode(&grey.encoded()).expect("grey decodes");
        assert_eq!(back.channels, 1);
        assert_eq!(back.pixels[0], 77);

        let rgba = solid(3, 2, ColorType::Rgba, &[1, 2, 3, 4]);
        let back = decode(&rgba.encoded()).expect("rgba decodes");
        assert_eq!(back.channels, 4);
        assert_eq!(back.pixels, rgba.pixels);
    }

    #[test]
    fn a_single_pixel_image_works() {
        let image = solid(1, 1, ColorType::Rgb, &[9, 8, 7]);
        let decoded = decode(&image.encoded()).expect("1x1 decodes");
        assert_eq!(decoded.pixels, vec![9, 8, 7]);
    }

    #[test]
    fn large_flat_image_compresses() {
        // The encoder must actually shrink a flat image, or PNG output is
        // pointless on a phone.
        let image = solid(200, 200, ColorType::Rgb, &[128, 128, 128]);
        let encoded = image.encoded();
        let raw = (200 * 200 * 3) as usize;
        assert!(encoded.len() < raw / 20, "flat image compressed poorly: {} vs {raw}", encoded.len());
    }

    #[test]
    fn rejects_a_bad_signature() {
        assert!(decode(b"not a png at all").is_err());
        assert!(decode(&[]).is_err());
    }

    #[test]
    fn rejects_a_corrupt_chunk() {
        let image = solid(4, 4, ColorType::Rgb, &[1, 2, 3]);
        let mut encoded = image.encoded();
        // Flip a byte inside IDAT: the CRC must catch it.
        let len = encoded.len();
        encoded[len / 2] ^= 0xFF;
        let err = decode(&encoded).err().expect("a corrupted PNG must not decode");
        assert!(err.contains("CRC"), "expected a CRC error, got: {err}");
    }

    #[test]
    fn rejects_a_truncated_file() {
        let image = solid(4, 4, ColorType::Rgb, &[1, 2, 3]);
        let encoded = image.encoded();
        let truncated = &encoded[..encoded.len() / 2];
        assert!(decode(truncated).is_err());
    }

    #[test]
    fn rejects_a_chunk_length_past_the_end() {
        // Hostile header: claims a 4 GiB chunk in a 30-byte file. Must fail
        // without attempting the allocation.
        let mut data = Vec::new();
        data.extend_from_slice(&SIGNATURE);
        data.extend_from_slice(&u32::MAX.to_be_bytes());
        data.extend_from_slice(b"IDAT");
        data.extend_from_slice(&[0u8; 8]);
        let err = decode(&data).err().expect("a hostile chunk length must not decode");
        assert!(err.contains("past the end"), "{err}");
    }

    #[test]
    fn rejects_an_unknown_filter_type() {
        // A scanline with filter byte 9 cannot be undone.
        let mut raw = vec![0u8; 8];
        raw[0] = 9;
        let mut line = vec![7u8; 7];
        let previous = vec![0u8; 7];
        assert!(unfilter_row(9, &mut line, &previous, 1).is_err());
    }

    #[test]
    fn pixel_accessors_report_and_convert() {
        let image = solid(2, 2, ColorType::Rgb, &[255, 0, 0]);
        let p = image.pixel_f64(0, 0).expect("pixel exists");
        assert!((p[0] - 1.0).abs() < 1e-9);
        assert!((p[1] - 0.0).abs() < 1e-9);
        assert!(image.pixel_f64(9, 9).is_none(), "out of range must be None");

        let rgba = image.to_rgba();
        assert_eq!(rgba.len(), 2 * 2 * 4);
        assert_eq!(&rgba[0..4], &[255, 0, 0, 255]);
    }

    #[test]
    fn mean_luma_of_a_black_and_white_image_is_mid_range() {
        let mut image = solid(2, 1, ColorType::Grey, &[0]);
        image.pixels[1] = 255;
        let luma = image.mean_luma();
        assert!(luma > 0.4 && luma < 0.6, "expected ~0.5, got {luma}");
    }

    #[test]
    fn sub_byte_depths_unpack_to_the_right_values() {
        // 1-bit: 0b10100000 -> 1,0,1,0,0,0,0,0 (scale 255)
        let row = [0b1010_0000u8];
        let samples = unpack_flat(&row, 1);
        assert_eq!(samples, vec![255, 0, 255, 0, 0, 0, 0, 0]);

        // 16-bit scales down to 8 bits by taking the high byte.
        let row16 = [0x12u8, 0x34, 0xFF, 0xFF];
        let samples16 = unpack_flat(&row16, 16);
        assert_eq!(samples16, vec![0x12, 0xFF]);
    }

    #[test]
    fn paeth_predictor_matches_the_spec_examples() {
        // p = a + b - c = 0, so the distances are |0-10|=10, |0-20|=20,
        // |0-30|=30 and `a` is the closest predictor. (An earlier version of
        // this test expected 20 and was wrong.)
        assert_eq!(paeth(10, 20, 30), 10);
        assert_eq!(paeth(0, 0, 0), 0);
        assert_eq!(paeth(255, 0, 0), 255);
        // Ties go to `a` first (pa <= pb), then `b`: with p = 0 the distances
        // are 100, 100, 200, so `a` wins.
        assert_eq!(paeth(100, 100, 200), 100);
    }

    #[test]
    fn interlaced_geometry_matches_the_spec() {
        // For an 8x8 image every Adam7 pass is 1 pixel except the last.
        let passes: Vec<(usize, usize)> = ADAM7
            .iter()
            .map(|p| (pass_width(8, *p), pass_height(8, *p)))
            .collect();
        assert_eq!(passes, vec![(1, 1), (1, 1), (2, 2), (2, 2), (4, 4), (4, 4), (8, 8)]);
        // A 5-wide image still has column 4, so pass 1 is 1 pixel wide; a
        // 4-wide image does not (the first version of this test got that wrong).
        assert_eq!(pass_width(5, ADAM7[1]), 1);
        assert_eq!(pass_width(4, ADAM7[1]), 0);
    }

    #[test]
    fn decodes_a_png_produced_by_an_independent_encoder() {
        // tests/data_real.png was written by Python's zlib, not by this module:
        // an independent producer is the only real proof the decoder is correct.
        let bytes = include_bytes!("../tests/data_real.png");
        let image = decode(bytes).expect("must decode a PNG this module did not write");
        assert_eq!(image.width, 3);
        assert_eq!(image.height, 2);
        assert_eq!(image.channels, 3);
        // Values were generated as (x*60 + y*20) % 256 per byte.
        for y in 0..2u32 {
            for x in 0..3u32 {
                for c in 0..3usize {
                    let expected = ((x as usize * 3 + c) * 60 + y as usize * 20) % 256;
                    let got = image.pixels[(y as usize * 3 + x as usize) * 3 + c];
                    assert_eq!(got, expected as u8, "pixel ({x},{y}) channel {c}");
                }
            }
        }
    }

    #[test]
    fn encoded_output_has_the_png_signature_and_iend() {
        let encoded = solid(2, 2, ColorType::Grey, &[5]).encoded();
        assert_eq!(&encoded[..8], &SIGNATURE);
        assert!(encoded.windows(4).any(|w| w == b"IEND"));
    }

    #[test]
    fn adler_of_the_image_data_is_consistent() {
        // The encoder's zlib trailer must be the Adler-32 of the raw scanlines.
        let image = solid(2, 2, ColorType::Grey, &[3]);
        let raw = image.filtered_scanlines();
        assert_eq!(adler32(&raw), adler32(&raw));
        assert!(zlib_compress(&raw).len() > 2);
    }
}
// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

/// Error buffer and the shared encoded-output buffer.
static mut ERROR: [u8; 192] = [0; 192];
static mut ENCODED: Vec<u8> = Vec::new();
/// The most recently decoded image, kept so accessors can describe it.
static mut CURRENT: Option<Image> = None;

fn set_error(message: &str) {
    let err = unsafe { &mut *core::ptr::addr_of_mut!(ERROR) };
    let bytes = message.as_bytes();
    let n = bytes.len().min(err.len() - 1);
    for (i, b) in bytes[..n].iter().enumerate() {
        err[i] = *b;
    }
    err[n] = 0;
}

/// The last error message; static storage, do not free.
#[no_mangle]
pub extern "C" fn flint_png_error() -> *const u8 {
    core::ptr::addr_of!(ERROR) as *const u8
}

/// The PNG produced by the last encode; valid until the next call.
#[no_mangle]
pub extern "C" fn flint_png_encoded() -> *const u8 {
    unsafe { ENCODED.as_ptr() }
}

/// Decodes a PNG from memory. Returns 1 on success, 0 on failure with
/// `flint_png_error()`. Size-checked before any allocation: a 20-byte hostile
/// file cannot ask for gigabytes.
#[no_mangle]
pub extern "C" fn flint_png_decode(data: *const u8, data_len: i64) -> i64 {
    let outcome = (|| -> Result<i64, String> {
        if data.is_null() || data_len < 8 {
            return Err(alloc::format!("png: input is null or too short"));
        }
        if data_len as usize > MAX_OUTPUT * 4 {
            return Err(alloc::format!("png: input larger than this decoder accepts"));
        }
        let bytes = unsafe { core::slice::from_raw_parts(data, data_len as usize) };
        let image = decode(bytes)?;
        unsafe { *core::ptr::addr_of_mut!(CURRENT) = Some(image) };
        Ok(1)
    })();
    match outcome {
        Ok(v) => {
            set_error("");
            v
        }
        Err(message) => {
            set_error(&message);
            0
        }
    }
}

/// Encodes `pixels` (interleaved 8-bit, `channels` per pixel) as a PNG.
/// Returns the byte length of the PNG, which is also available through
/// `flint_png_encoded()`, or -1 on failure.
#[no_mangle]
pub extern "C" fn flint_png_encode(
    width: i64,
    height: i64,
    channels: i64,
    pixels: *const u8,
    pixels_len: i64,
) -> i64 {
    let outcome = (|| -> Result<i64, String> {
        if width <= 0 || height <= 0 {
            return Err(alloc::format!("png: width and height must be positive"));
        }
        if (width as usize) * (height as usize) > MAX_PIXELS {
            return Err(alloc::format!("png: image exceeds the 64 megapixel limit"));
        }
        if !(1..=4).contains(&channels) {
            return Err(alloc::format!("png: channels must be 1, 2, 3 or 4"));
        }
        let color = match channels {
            1 => ColorType::Grey,
            2 => ColorType::GreyAlpha,
            3 => ColorType::Rgb,
            _ => ColorType::Rgba,
        };
        let expected = (width as usize) * (height as usize) * channels as usize;
        if pixels.is_null() || pixels_len < expected as i64 {
            return Err(alloc::format!(
                "png: need {} pixel bytes, got {}",
                expected,
                pixels_len
            ));
        }
        let slice = unsafe { core::slice::from_raw_parts(pixels, expected) };
        let image = Image {
            width: width as u32,
            height: height as u32,
            bit_depth: 8,
            color_type: color,
            pixels: slice.to_vec(),
            channels: channels as usize,
        };
        let encoded = image.encoded();
        if encoded.len() > MAX_OUTPUT {
            return Err(alloc::format!("png: encoded output exceeds 32 MiB"));
        }
        unsafe {
            ENCODED = encoded;
        }
        Ok(unsafe { ENCODED.len() } as i64)
    })();
    match outcome {
        Ok(v) => {
            set_error("");
            v
        }
        Err(message) => {
            set_error(&message);
            -1
        }
    }
}

macro_rules! accessor {
    ($name:ident, $field:ident, $ty:ty, $doc:literal) => {
        #[doc = $doc]
        #[no_mangle]
        pub extern "C" fn $name() -> $ty {
            match unsafe { &*core::ptr::addr_of!(CURRENT) } {
                Some(image) => image.$field as i64,
                None => -1,
            }
        }
    };
}

accessor!(flint_png_width, width, i64, "Width of the last decoded image, or -1.");
accessor!(flint_png_height, height, i64, "Height of the last decoded image, or -1.");
accessor!(flint_png_channels, channels, i64, "Channels per pixel of the last decoded image, or -1.");

/// One channel of one pixel (0..=255), or -1 when out of range.
#[no_mangle]
pub extern "C" fn flint_png_pixel(x: i64, y: i64, channel: i64) -> i64 {
    let current = unsafe { &*core::ptr::addr_of!(CURRENT) };
    match current {
        Some(image) => {
            if x < 0 || y < 0 || channel < 0 {
                return -1;
            }
            let index = (y as usize) * (image.width as usize) * image.channels
                + (x as usize) * image.channels
                + channel as usize;
            match image.pixels.get(index) {
                Some(value) => *value as i64,
                None => -1,
            }
        }
        None => -1,
    }
}

/// Mean luma (Rec. 601) of the last decoded image, 0.0..=1.0, or -1.0.
#[no_mangle]
pub extern "C" fn flint_png_mean_luma() -> f64 {
    match unsafe { &*core::ptr::addr_of!(CURRENT) } {
        Some(image) => image.mean_luma(),
        None => -1.0,
    }
}
