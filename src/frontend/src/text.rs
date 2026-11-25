//! Texto: shaping (`rustybuzz`), metricas/raster (`ab_glyph`) y atlas R8.
//!
//! Reproduce `src/frontend/python/webnative/text.py` en lo que define el
//! contrato del IR: mismos gids, mismos avances (redondeados a 26.6 como
//! HarfBuzz), mismas posiciones de quad y mismo atlas shelf 512x512 con 1 px
//! de guarda. La cobertura de cada pixel la produce `ab_glyph` (sin TrueType
//! hinting), asi que los bytes del atlas no son identicos a los de FreeType.
//!
//! Ajuste de extensiones: FreeType con `FT_LOAD_TARGET_NORMAL` aplica el
//! interprete TrueType y "encaja" las alturas en la rejilla de pixeles. Aqui
//! se aproxima de forma determinista redondeando los extremos verticales del
//! contorno (`round(ymax)`, `round(ymin)`), que para Segoe UI 28 px reproduce
//! exactamente `bitmap_top`/`rows` de FreeType. El bitmap se recorta a esa caja
//! entera (se pierde el overshoot subpixel de 'o'/'a', ~0.33 px).

use std::collections::HashMap;
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use ab_glyph::{Font, FontRef, GlyphId as AbGlyphId, PxScale};
use rustybuzz::{Direction, Face, Language, UnicodeBuffer};

use crate::layout::TextRun;

pub const ATLAS_SIZE: u32 = 512;

const GENERIC: [(&str, &str); 6] = [
    ("sans-serif", "segoe ui"),
    ("system-ui", "segoe ui"),
    ("-apple-system", "segoe ui"),
    ("ui-sans-serif", "segoe ui"),
    ("serif", "times new roman"),
    ("monospace", "consolas"),
];

const FONT_FAMILIES: [(&str, &str); 14] = [
    ("segoe ui", "segoeui.ttf"),
    ("segoe ui light", "segoeuil.ttf"),
    ("segoe ui semibold", "seguisb.ttf"),
    ("segoe ui semilight", "segoeuisl.ttf"),
    ("arial", "arial.ttf"),
    ("helvetica", "arial.ttf"),
    ("helvetica neue", "arial.ttf"),
    ("tahoma", "tahoma.ttf"),
    ("verdana", "verdana.ttf"),
    ("calibri", "calibri.ttf"),
    ("times new roman", "times.ttf"),
    ("georgia", "georgia.ttf"),
    ("courier new", "cour.ttf"),
    ("consolas", "consola.ttf"),
];

const DEFAULT_FAMILY: &str = "segoe ui";

/// Variantes reales en negrita por familia base (E1). Un `font-weight >= 600`
/// usa estos TTF; si el sistema no los tiene se cae al regular con diagnostico.
const BOLD_FILES: [(&str, &str); 14] = [
    ("segoe ui", "segoeuib.ttf"),
    ("segoe ui light", "segoeuib.ttf"),
    ("segoe ui semilight", "segoeuib.ttf"),
    ("segoe ui semibold", "segoeuib.ttf"),
    ("arial", "arialbd.ttf"),
    ("helvetica", "arialbd.ttf"),
    ("helvetica neue", "arialbd.ttf"),
    ("tahoma", "tahomabd.ttf"),
    ("verdana", "verdanab.ttf"),
    ("calibri", "calibrib.ttf"),
    ("times new roman", "timesbd.ttf"),
    ("georgia", "georgiab.ttf"),
    ("courier new", "courbd.ttf"),
    ("consolas", "consolab.ttf"),
];

/// `font-weight` CSS -> 100..900. `bold`/`normal` se aceptan como alias.
pub fn parse_font_weight(value: &str) -> u16 {
    let text = value.trim().to_ascii_lowercase();
    match text.as_str() {
        "bold" | "bolder" => 700,
        "normal" | "lighter" => 400,
        _ => text.parse::<u16>().unwrap_or(400).clamp(100, 900),
    }
}

fn windows_fonts() -> PathBuf {
    let windir = std::env::var("WINDIR").unwrap_or_else(|_| "C:\\Windows".to_string());
    PathBuf::from(windir).join("Fonts")
}

/// Primera familia de una lista CSS: `"Segoe UI", Arial, sans-serif`.
pub fn first_family(font_family: &str) -> String {
    let first = font_family
        .split(',')
        .next()
        .unwrap_or("")
        .trim()
        .trim_matches(|c| c == '\'' || c == '"');
    if first.is_empty() {
        DEFAULT_FAMILY.to_string()
    } else {
        first.to_string()
    }
}

/// `font-family` CSS -> TTF existente, con el mismo mapa y fallback que Python.
pub fn resolve_font_file(font_family: &str) -> Result<PathBuf, String> {
    Ok(resolve_font_file_weighted(font_family, 400)?.0)
}

/// `font-family` + `font-weight` -> TTF existente.
///
/// Devuelve `(ruta, fallback)`; `fallback` es `true` cuando se pidio negrita
/// pero la variante no existe y se usa el regular (el llamador avisa una vez).
pub fn resolve_font_file_weighted(
    font_family: &str,
    weight: u16,
) -> Result<(PathBuf, bool), String> {
    let family = first_family(font_family).to_ascii_lowercase();
    let family = GENERIC
        .iter()
        .find(|(name, _)| *name == family)
        .map(|(_, mapped)| *mapped)
        .unwrap_or(family.as_str());
    let fonts = windows_fonts();

    if weight >= 600 {
        if let Some((_, filename)) = BOLD_FILES.iter().find(|(name, _)| *name == family) {
            let path = fonts.join(filename);
            if path.is_file() {
                return Ok((path, false));
            }
        }
    }

    let regular = resolve_font_file_regular(font_family)?;
    Ok((regular, weight >= 600))
}

fn resolve_font_file_regular(font_family: &str) -> Result<PathBuf, String> {
    let family = first_family(font_family).to_ascii_lowercase();
    let family = GENERIC
        .iter()
        .find(|(name, _)| *name == family)
        .map(|(_, mapped)| *mapped)
        .unwrap_or(family.as_str());
    let fonts = windows_fonts();

    if let Some((_, filename)) = FONT_FAMILIES.iter().find(|(name, _)| *name == family) {
        let path = fonts.join(filename);
        if path.is_file() {
            return Ok(path);
        }
    }

    // Coincidencia parcial: "Segoe UI Semibold", "Arial Black", etc.
    let mut names: Vec<&(&str, &str)> = FONT_FAMILIES.iter().collect();
    names.sort_by_key(|(name, _)| std::cmp::Reverse(name.len()));
    for (name, filename) in names {
        if family.contains(name) {
            let path = fonts.join(filename);
            if path.is_file() {
                return Ok(path);
            }
        }
    }

    for fallback in ["segoeui.ttf", "arial.ttf"] {
        let path = fonts.join(fallback);
        if path.is_file() {
            return Ok(path);
        }
    }
    Err(format!(
        "no hay fuente usable para {font_family:?}: faltan {} y {}",
        fonts.join("segoeui.ttf").display(),
        fonts.join("arial.ttf").display()
    ))
}

#[derive(Clone)]
pub struct GlyphBitmap {
    pub w: i32,
    pub h: i32,
    pub left: i32,
    pub top: i32,
    /// `w * h` bytes de cobertura, fila a fila.
    pub pixels: Vec<u8>,
}

impl GlyphBitmap {
    fn empty() -> Self {
        GlyphBitmap {
            w: 0,
            h: 0,
            left: 0,
            top: 0,
            pixels: Vec::new(),
        }
    }
}

pub struct ShapedGlyph {
    pub gid: u16,
    pub adv: f64,
    pub dx: f64,
    pub dy: f64,
}

struct FontFace {
    path: PathBuf,
    data: Vec<u8>,
    upem: i64,
    ascender: i64,
    descender: i64,
    /// Cache de metricas por tamano de pixel redondeado.
    metrics: RefCell<HashMap<i32, (f64, f64)>>,
    /// Cache de bitmaps por `(gid, tamano de pixel redondeado)`.
    bitmaps: RefCell<HashMap<(u16, i32), GlyphBitmap>>,
}

impl FontFace {
    fn load(path: &Path) -> Result<Self, String> {
        let data = std::fs::read(path).map_err(|err| format!("{}: {err}", path.display()))?;
        let face = Face::from_slice(&data, 0)
            .ok_or_else(|| format!("fuente invalida: {}", path.display()))?;
        if FontRef::try_from_slice(&data).is_err() {
            return Err(format!("fuente invalida para rasterizar: {}", path.display()));
        }
        Ok(FontFace {
            path: path.to_path_buf(),
            upem: i64::from(face.units_per_em()),
            ascender: i64::from(face.ascender()),
            descender: i64::from(face.descender()),
            data,
            metrics: RefCell::new(HashMap::new()),
            bitmaps: RefCell::new(HashMap::new()),
        })
    }

    fn path_string(&self) -> String {
        self.path.to_string_lossy().to_string()
    }

    /// `(ascender, descender)` en px, replicando `FT_Request_Metrics`:
    /// escala 16.16 con `FT_DivFix`, `FT_MulFix` y `FT_PIX_CEIL`/`FT_PIX_FLOOR`.
    fn metrics(&self, size: f64) -> (f64, f64) {
        // `int(round(size))` de Python: redondeo a par mas cercano.
        let pixel_size = (size.round_ties_even() as i32).max(1);
        if let Some(cached) = self.metrics.borrow().get(&pixel_size) {
            return *cached;
        }
        let y_scale = div_fix_16_16(i64::from(pixel_size) * 64, self.upem);
        let ascender = pix_ceil(mul_fix(self.ascender, y_scale));
        let descender = pix_floor(mul_fix(self.descender, y_scale));
        let result = (
            ascender as f64 / 64.0,
            -(descender as f64) / 64.0,
        );
        self.metrics.borrow_mut().insert(pixel_size, result);
        result
    }

    fn hb_scale(&self, size: f64) -> i64 {
        ((size * 64.0).round_ties_even() as i64).max(1)
    }

    fn shape(&self, text: &str, size: f64) -> Vec<ShapedGlyph> {
        let face = Face::from_slice(&self.data, 0).expect("fuente validada al cargar");
        let mut buffer = UnicodeBuffer::new();
        buffer.push_str(text);
        buffer.set_direction(Direction::LeftToRight);
        buffer.set_script(rustybuzz::script::LATIN);
        if let Ok(language) = "en".parse::<Language>() {
            buffer.set_language(language);
        }
        let output = rustybuzz::shape(&face, &[], buffer);
        let scale = self.hb_scale(size);
        output
            .glyph_infos()
            .iter()
            .zip(output.glyph_positions().iter())
            .map(|(info, pos)| ShapedGlyph {
                gid: info.glyph_id as u16,
                adv: scale_26_6(pos.x_advance, scale, self.upem),
                dx: scale_26_6(pos.x_offset, scale, self.upem),
                dy: scale_26_6(pos.y_offset, scale, self.upem),
            })
            .collect()
    }

    /// Bitmap de cobertura con la caja entera que usaria FreeType, salvo que
    /// los extremos verticales se redondean para emular el hinting vertical.
    fn raster(&self, gid: u16, size: f64) -> GlyphBitmap {
        // `max(1, round(size))` en pixeles, con el redondeo de Python.
        let pixel_size = (size.round_ties_even() as i32).max(1);
        if let Some(cached) = self.bitmaps.borrow().get(&(gid, pixel_size)) {
            return cached.clone();
        }
        let bitmap = self.raster_uncached(gid, pixel_size);
        self.bitmaps
            .borrow_mut()
            .insert((gid, pixel_size), bitmap.clone());
        bitmap
    }

    fn raster_uncached(&self, gid: u16, pixel_size: i32) -> GlyphBitmap {
        let font = FontRef::try_from_slice(&self.data).expect("fuente validada al cargar");
        let upem = font.units_per_em().expect("fuente escalable");
        let outline = match font.outline(AbGlyphId(gid)) {
            Some(outline) => outline,
            None => return GlyphBitmap::empty(),
        };
        let px_scale = PxScale::from(pixel_size as f32 * font.height_unscaled() / upem);
        let scale_factor = px_scale.y / font.height_unscaled();
        let glyph = AbGlyphId(gid).with_scale(px_scale);
        let outlined = match font.outline_glyph(glyph) {
            Some(outlined) => outlined,
            None => return GlyphBitmap::empty(),
        };

        let bounds = outline.bounds;
        let xmin = bounds.min.x * scale_factor;
        let xmax = bounds.max.x * scale_factor;
        // En `ab_glyph` el rect del contorno guarda y en unidades de fuente con
        // el eje hacia arriba, por eso `min.y` es el extremo superior.
        let ymax = bounds.min.y * scale_factor;
        let ymin = bounds.max.y * scale_factor;

        let left = xmin.floor() as i32;
        let right = xmax.ceil() as i32;
        // Redondeo vertical: aproximacion a TARGET_NORMAL (ver doc del modulo).
        let top = ymax.round() as i32;
        let bottom = ymin.round() as i32;
        let w = right - left;
        let h = top - bottom;
        if w <= 0 || h <= 0 {
            return GlyphBitmap::empty();
        }

        let px_bounds = outlined.px_bounds();
        let mut pixels = vec![0u8; (w * h) as usize];
        outlined.draw(|x, y, coverage| {
            let dest_x = x as i32 + px_bounds.min.x as i32 - left;
            let dest_y = y as i32 + px_bounds.min.y as i32 + top;
            if dest_x < 0 || dest_y < 0 || dest_x >= w || dest_y >= h {
                return;
            }
            let value = (coverage.clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
            pixels[(dest_y * w + dest_x) as usize] = value;
        });

        GlyphBitmap {
            w,
            h,
            left,
            top,
            pixels,
        }
    }
}

/// `FT_MulFix`: `(a * b + 0x8000) >> 16` (redondeo con shift aritmetico).
fn mul_fix(value: i64, scale_16_16: i64) -> i64 {
    (value * scale_16_16 + 0x8000) >> 16
}

/// `FT_DivFix`: valor 26.6 -> escala 16.16 con redondeo.
fn div_fix_16_16(value_26_6: i64, upem: i64) -> i64 {
    (value_26_6 * 65536 + upem / 2) / upem
}

fn pix_ceil(value: i64) -> i64 {
    (value + 63) & !63
}

fn pix_floor(value: i64) -> i64 {
    value & !63
}

/// Avance de HarfBuzz en unidades de fuente -> px con redondeo a 26.6.
fn scale_26_6(units: i32, scale: i64, upem: i64) -> f64 {
    let numerator = i64::from(units) * scale;
    let rounded = if numerator >= 0 {
        (numerator + upem / 2) / upem
    } else {
        (numerator - upem / 2) / upem
    };
    rounded as f64 / 64.0
}

pub struct TextEngine {
    faces: RefCell<HashMap<String, Rc<FontFace>>>,
    /// Diagnosticos ya emitidos (una linea por familia/peso sin variante).
    warned: RefCell<std::collections::HashSet<String>>,
}

impl TextEngine {
    pub fn new() -> Self {
        TextEngine {
            faces: RefCell::new(HashMap::new()),
            warned: RefCell::new(std::collections::HashSet::new()),
        }
    }

    fn face(&self, path: &Path) -> Result<Rc<FontFace>, String> {
        let key = path.to_string_lossy().to_string();
        if let Some(face) = self.faces.borrow().get(&key) {
            return Ok(face.clone());
        }
        let face = Rc::new(FontFace::load(path)?);
        self.faces.borrow_mut().insert(key, face.clone());
        Ok(face)
    }

    fn face_for_family(&self, font_family: &str) -> Result<Rc<FontFace>, String> {
        self.face(&resolve_font_file(font_family)?)
    }

    /// Igual que `face_for_family`, eligiendo la variante real de `font-weight`.
    /// Si la negrita no existe se usa el regular y se emite un diagnostico una
    /// sola vez por familia/peso (no altera la salida ni el contrato del IR).
    fn face_for_family_weighted(
        &self,
        font_family: &str,
        weight: u16,
    ) -> Result<Rc<FontFace>, String> {
        let (path, fallback) = resolve_font_file_weighted(font_family, weight)?;
        if fallback {
            let message = format!(
                "font-weight {weight} sin variante real para {font_family:?}; se usa {}",
                path.display()
            );
            if self.warned.borrow_mut().insert(message.clone()) {
                eprintln!("frontend-rust: {message}");
            }
        }
        self.face(&path)
    }

    pub fn metrics(&self, font_family: &str, size: f64) -> Result<(f64, f64), String> {
        Ok(self.face_for_family(font_family)?.metrics(size))
    }

    fn raster_path(&self, path: &str, gid: u16, size: f64) -> Result<GlyphBitmap, String> {
        Ok(self.face(Path::new(path))?.raster(gid, size))
    }
}

fn size_key(size: f64) -> i32 {
    (size.round_ties_even() as i32).max(1)
}

pub struct Atlas {
    size: u32,
    pub data: Vec<u8>,
    rects: HashMap<(String, u16, i32), Option<(u32, u32, u32, u32)>>,
    shelf_x: u32,
    shelf_y: u32,
    shelf_h: u32,
}

impl Atlas {
    pub fn new(size: u32) -> Self {
        Atlas {
            size,
            data: vec![0; (size * size) as usize],
            rects: HashMap::new(),
            shelf_x: 0,
            shelf_y: 0,
            shelf_h: 0,
        }
    }

    fn add(&mut self, key: (String, u16, i32), bitmap: &GlyphBitmap) -> Result<(), String> {
        if self.rects.contains_key(&key) {
            return Ok(());
        }
        if bitmap.w <= 0 || bitmap.h <= 0 {
            self.rects.insert(key, None);
            return Ok(());
        }
        let w = bitmap.w as u32;
        let h = bitmap.h as u32;
        // 1 px de guarda contra el sangrado del sampler linear.
        let pad_w = w + 2;
        let pad_h = h + 2;
        if self.shelf_x + pad_w > self.size {
            self.shelf_x = 0;
            self.shelf_y += self.shelf_h;
            self.shelf_h = 0;
        }
        if self.shelf_y + pad_h > self.size {
            return Err(format!(
                "atlas de glyphs desbordado al añadir {:?}",
                key
            ));
        }
        let x = self.shelf_x + 1;
        let y = self.shelf_y + 1;
        for row in 0..h {
            let src = (row * w) as usize;
            let dest = ((y + row) * self.size + x) as usize;
            self.data[dest..dest + w as usize]
                .copy_from_slice(&bitmap.pixels[src..src + w as usize]);
        }
        self.rects.insert(key, Some((x, y, w, h)));
        self.shelf_x += pad_w;
        self.shelf_h = self.shelf_h.max(pad_h);
        Ok(())
    }

    fn uv(&self, key: &(String, u16, i32)) -> (f64, f64, f64, f64) {
        match self.rects.get(key) {
            Some(Some((x, y, w, h))) => {
                let scale = f64::from(self.size);
                (
                    f64::from(*x) / scale,
                    f64::from(*y) / scale,
                    f64::from(*x + *w) / scale,
                    f64::from(*y + *h) / scale,
                )
            }
            _ => (0.0, 0.0, 0.0, 0.0),
        }
    }

    pub fn width(&self) -> u32 {
        self.size
    }

    pub fn height(&self) -> u32 {
        self.size
    }

    pub fn glyph_count(&self) -> usize {
        self.rects.values().filter(|rect| rect.is_some()).count()
    }

    pub fn pixels_nonzero(&self) -> usize {
        self.data.iter().filter(|byte| **byte != 0).count()
    }
}

pub struct GlyphOut {
    pub gid: u16,
    pub x: f64,
    pub y: f64,
    pub w: i32,
    pub h: i32,
    pub u0: f64,
    pub v0: f64,
    pub u1: f64,
    pub v1: f64,
    pub adv: f64,
}

pub struct TextOp {
    pub x: f64,
    pub y: f64,
    pub text: String,
    pub font: String,
    pub font_file: String,
    pub size: f64,
    pub color: String,
    pub glyphs: Vec<GlyphOut>,
}

fn round_to(value: f64, digits: i32) -> f64 {
    let factor = 10f64.powi(digits);
    (value * factor).round_ties_even() / factor
}

pub type BuildTextResult = (Vec<TextOp>, Option<Atlas>);

/// `TextRun` (layout) -> `TextOp` (IR) + atlas, en orden de primera aparicion.
pub fn build_text_ops(runs: &[TextRun], engine: &TextEngine) -> Result<BuildTextResult, String> {
    if runs.is_empty() {
        return Ok((Vec::new(), None));
    }

    struct Shaped {
        run: TextRun,
        path: String,
        glyphs: Vec<ShapedGlyph>,
    }

    let mut shaped: Vec<Shaped> = Vec::with_capacity(runs.len());
    let mut keys: Vec<(String, u16, i32)> = Vec::new();
    let mut seen: std::collections::HashSet<(String, u16, i32)> = std::collections::HashSet::new();
    for run in runs {
        let face = engine.face_for_family(&run.font_family)?;
        let glyphs = face.shape(&run.text, run.font_size);
        let path = face.path_string();
        let key_size = size_key(run.font_size);
        for glyph in &glyphs {
            let key = (path.clone(), glyph.gid, key_size);
            if seen.insert(key.clone()) {
                keys.push(key);
            }
        }
        shaped.push(Shaped {
            run: run.clone(),
            path,
            glyphs,
        });
    }

    let mut atlas = Atlas::new(ATLAS_SIZE);
    for key in &keys {
        let bitmap = engine.raster_path(&key.0, key.1, f64::from(key.2))?;
        atlas.add(key.clone(), &bitmap)?;
    }

    let mut ops: Vec<TextOp> = Vec::with_capacity(shaped.len());
    for item in shaped {
        let run = item.run;
        let key_size = size_key(run.font_size);
        let mut pen = run.x;
        let mut glyphs: Vec<GlyphOut> = Vec::with_capacity(item.glyphs.len());
        for glyph in &item.glyphs {
            let key = (item.path.clone(), glyph.gid, key_size);
            let bitmap = engine.raster_path(&key.0, key.1, f64::from(key.2))?;
            let (u0, v0, u1, v1) = atlas.uv(&key);
            glyphs.push(GlyphOut {
                gid: glyph.gid,
                x: round_to(pen + glyph.dx + f64::from(bitmap.left), 3),
                y: round_to(run.baseline - glyph.dy - f64::from(bitmap.top), 3),
                w: bitmap.w,
                h: bitmap.h,
                u0: round_to(u0, 6),
                v0: round_to(v0, 6),
                u1: round_to(u1, 6),
                v1: round_to(v1, 6),
                adv: round_to(glyph.adv, 4),
            });
            pen += glyph.adv;
        }
        ops.push(TextOp {
            x: round_to(run.x, 3),
            y: round_to(run.baseline, 3),
            text: run.text,
            font: first_family(&run.font_family),
            font_file: item.path,
            size: round_to(run.font_size, 3),
            color: run.color,
            glyphs,
        });
    }

    Ok((ops, Some(atlas)))
}

// ------------------------------------------------- R2 pre-layout style tree
//
// `emit_ui.py` del oraculo Python mide y hornea el texto de otra forma que el
// IR de R1: el atlas es 256x256, las posiciones son relativas a la caja del
// nodo de texto (sin redondear a 3 decimales) y cada run guarda el ascender
// como baseline. Esta API reproduce esa semantica sin tocar `build_text_ops`.

/// Tamano del atlas del contrato R2 (`emit_ui.py` usa 256).
pub const UI_ATLAS_SIZE: u32 = 256;

/// Las paginas con `kind:"input"` hornean la piscina ASCII imprimible
/// (32..126) por tamano de fuente; 512 evita que la piscina desplace el atlas
/// de texto existente. Las paginas sin input siguen en 256 (byte-identicas).
pub const UI_INPUT_ATLAS_SIZE: u32 = 512;

/// Glyph ya posicionado relativo a la esquina superior izquierda del texto.
pub struct UiGlyphOut {
    pub gid: u16,
    /// Avance HarfBuzz del glyph (no lo consume R2, si la verificacion).
    pub advance: f64,
    pub x: f64,
    pub y: f64,
    pub w: i32,
    pub h: i32,
    pub u0: f64,
    pub v0: f64,
    pub u1: f64,
    pub v1: f64,
}

/// Resultado de hornear un texto: glyphs + metricas de la linea.
pub struct BakedUiText {
    pub glyphs: Vec<UiGlyphOut>,
    /// Suma de avances (lo que `emit_ui.TextMetrics.measure` llama width).
    pub width: f64,
    pub ascender: f64,
    pub descender: f64,
}

/// Atlas R8 incremental para el arbol de estilos (mismo shelf packer que
/// `GlyphPacker` del oraculo: 256x256, guarda de 1 px, orden de aparicion).
pub struct UiTextBaker {
    atlas: Atlas,
}

impl UiTextBaker {
    #[allow(dead_code)]
    pub fn new() -> Self {
        UiTextBaker {
            atlas: Atlas::new(UI_ATLAS_SIZE),
        }
    }

    /// Atlas de 512x512 para paginas con `kind:"input"`: la piscina ASCII
    /// imprimible (95 glyphs por tamano de fuente) no debe empujar el texto
    /// fuera del atlas de 256x256.
    pub fn with_size(size: u32) -> Self {
        UiTextBaker {
            atlas: Atlas::new(size),
        }
    }

    pub fn atlas(&self) -> &Atlas {
        &self.atlas
    }

    /// Shape + raster + empaquetado de un texto, en el orden exacto del
    /// oraculo: se avanza el pen con todos los glyphs y solo se registra el
    /// glyph cuando su bitmap tiene area.
    pub fn bake(
        &mut self,
        engine: &TextEngine,
        text: &str,
        font_family: &str,
        size: f64,
        weight: u16,
    ) -> Result<BakedUiText, String> {
        let face = engine.face_for_family_weighted(font_family, weight)?;
        let (ascender, descender) = face.metrics(size);
        let shaped = face.shape(text, size);
        let path = face.path_string();
        let key_size = size_key(size);
        let mut glyphs: Vec<UiGlyphOut> = Vec::with_capacity(shaped.len());
        let mut pen = 0.0f64;
        let mut width = 0.0f64;
        for item in &shaped {
            width += item.adv;
            let bitmap = face.raster(item.gid, size);
            if bitmap.w > 0 && bitmap.h > 0 {
                let key = (path.clone(), item.gid, key_size);
                self.atlas.add(key.clone(), &bitmap)?;
                let (u0, v0, u1, v1) = self.atlas.uv(&key);
                glyphs.push(UiGlyphOut {
                    gid: item.gid,
                    advance: item.adv,
                    x: pen + item.dx + f64::from(bitmap.left),
                    y: ascender - item.dy - f64::from(bitmap.top),
                    w: bitmap.w,
                    h: bitmap.h,
                    u0,
                    v0,
                    u1,
                    v1,
                });
            }
            pen += item.adv;
        }
        Ok(BakedUiText {
            glyphs,
            width,
            ascender,
            descender,
        })
    }

    /// Piscina ASCII imprimible (32..126) para un `kind:"input"`: un glyph por
    /// codepoint, con avance HarfBuzz aislado, caja relativa al top-left del
    /// content box y UVs del atlas. El runtime compone el texto sumando
    /// avances; no hay shaping en runtime (sin kerning). La piscina se comparte
    /// por `(familia, tamano)` y el atlas deduplica por `(path, gid, size)`.
    pub fn bake_input_pool(
        &mut self,
        engine: &TextEngine,
        font_family: &str,
        size: f64,
        weight: u16,
    ) -> Result<BakedInputPool, String> {
        let face = engine.face_for_family_weighted(font_family, weight)?;
        let (ascender, descender) = face.metrics(size);
        let path = face.path_string();
        let key_size = size_key(size);
        let mut glyphs = Vec::with_capacity(95);
        for cp in 32u32..=126 {
            let character = char::from_u32(cp).expect("ASCII imprimible es un scalar valido");
            let mut buffer = [0u8; 4];
            let shaped = face.shape(character.encode_utf8(&mut buffer), size);
            let (gid, advance, dx, dy) = match shaped.first() {
                Some(item) => (item.gid, item.adv, item.dx, item.dy),
                None => (0, 0.0, 0.0, 0.0),
            };
            let bitmap = face.raster(gid, size);
            let (u0, v0, u1, v1) = if bitmap.w > 0 && bitmap.h > 0 {
                let key = (path.clone(), gid, key_size);
                self.atlas.add(key.clone(), &bitmap)?;
                self.atlas.uv(&key)
            } else {
                (0.0, 0.0, 0.0, 0.0)
            };
            glyphs.push(UiInputGlyphOut {
                cp,
                gid,
                advance,
                x: dx + f64::from(bitmap.left),
                y: ascender - dy - f64::from(bitmap.top),
                w: bitmap.w,
                h: bitmap.h,
                u0,
                v0,
                u1,
                v1,
            });
        }
        Ok(BakedInputPool {
            glyphs,
            ascender,
            descender,
        })
    }
}

/// Glyph de la piscina ASCII de un input. `gid` es diagnostico; el runtime
/// solo usa avance, caja y UVs.
#[derive(Clone)]
pub struct UiInputGlyphOut {
    pub cp: u32,
    pub gid: u16,
    pub advance: f64,
    pub x: f64,
    pub y: f64,
    pub w: i32,
    pub h: i32,
    pub u0: f64,
    pub v0: f64,
    pub u1: f64,
    pub v1: f64,
}

#[derive(Clone)]
pub struct BakedInputPool {
    pub glyphs: Vec<UiInputGlyphOut>,
    pub ascender: f64,
    pub descender: f64,
}
