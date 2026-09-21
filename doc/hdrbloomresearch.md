# ⚠ REVISED DIRECTIVE (2026-08-20): the frosted-acrylic diffuser model

> This section supersedes parts of the problem statement below and the earlier
> acceptance criteria. The user reviewed the promoted `norm-tonescale-guarded`
> renders and rejected the target they optimize for. Read this before acting on
> anything else in this issue.

## The correct physical model

Stop thinking of this as *camera/eye bloom around bright points*. The target is
**LEDs embedded behind a frosted acrylic light diffuser** — the milky panel of
a commercial LED sign. That material's behavior is what "bloom" must mean here:

- **Diffusion grows with drive level.** A dim LED reads as a soft dot behind
  the acrylic. As it brightens, more energy reaches the wide scatter lobe and
  the glow spreads across neighboring LEDs. Apparent diffusion is a strong,
  monotonic function of local brightness.
- **Full white is a white-out.** A fully driven white region must bleed into a
  continuous glowing pane — individual LED dots merge and detail inside the
  region washes away. This is the *desired* look, not a failure to protect
  against. "Preserve the sharp base" is **wrong at high drive levels**; sharp,
  separated dots inside a full-white area are exactly what the real material
  does not show.
- **Color still survives as color.** A saturated red LED glows red through the
  acrylic at any brightness; the diffuser scatters the light it is given, it
  does not whiten hue. Whitening remains a defect for *colored* light — the
  hue-preservation work in this issue stands.
- **Dark stays dark.** Unlit acrylic is not luminous. Deep blacks keep their
  black; there is no ambient veil. The diffusion response applies to emitted
  light only.

## Why every strategy to date fails this

All prior rounds — including the currently shipped `norm-tonescale-guarded` —
optimized "genuinely white highlights still bloom *fully*" while also
maximizing core sharpness and treating bloom as a bounded additive halo. Under
the acrylic model that target is too timid in one direction and wrong in
another:

1. White regions in the demos show **almost no visible bloom**: the tonescale
   compresses the halo right where the acrylic would be flaring hardest. G1
   (+3.3 mean luma at the white peak) passes the old gate but is nowhere near
   a white-out.
2. **Core sharpness (S3) is scored as a virtue at all brightness levels.** In
   the acrylic model it is a virtue only in dim-to-mid content; at high drive
   the dots must merge, so a high S3 on bright regions is a *defect*.

## Revised acceptance behavior

- Dim content: soft dots, mild halo, hue preserved — roughly current behavior.
- Mid brightness: visible glow spreading toward neighbors, hue preserved.
- Bright saturated color: wide colored flare — red floods red across the
  region, never whitening.
- **Bright white: white-out.** The region reads as one continuous glowing
  surface; LED separation largely disappears; edges of the region flare
  outward into adjacent dark panel.
- Unlit panel: black, no veil.

## Metric implications (bloom_metrics.py must change before the next round)

- **G1 becomes a diffusion-response curve, not a threshold.** Measure added
  glow as a function of local drive level; the curve must rise steeply at the
  top end. A flat response (bounded halo) fails even if positive.
- **S3 becomes brightness-conditional.** Core similarity stays a score in
  dim/mid regions and inverts into a *merge requirement* inside bright
  regions: within a region of near-full drive, dot-to-gap contrast should
  approach zero.
- **New white-out metric:** in a bright white region, ratio of inter-LED gap
  luminance to LED-core luminance; target approaches 1.0. On fluid_eyes3's
  t≈2.4s peak this should look like a bloom of light, not a cluster of dots.
- G2 (no veil on genuinely dark pixels) and S1/S2 (hue fidelity and chroma
  retention of added light) survive unchanged — they encode the parts of the
  old directive that were correct.

## The reference look already exists: legacy default bloom

The plain legacy additive bloom (the pre-HDR UnrealBloom composite) **modeled
the acrylic diffusion really well**: glow grows steeply with brightness and a
fully driven region whites out into one pane, exactly as the material does.
It had exactly one defect: **past the bloom threshold, pure red/green/blue
turned white at the bright spots**, because additive bloom over an
already-bright core overflows and clips each RGB channel independently —
whichever channels hit 1.0 first are lost and the residue is white.

So the next round is NOT "invent a new diffusion model". It is:

> **Keep legacy bloom's energy and spatial response verbatim; change only what
> happens past the threshold.** Past-threshold overflow must behave like a
> diffuser instead of a clip: hold the pixel's hue ratio fixed (norm-based
> limiting, the one thing `norm-tonescale-guarded` got right) and let the
> excess energy go *outward* — into a wider scatter radius — rather than
> *upward* into per-channel clipping. A blown red LED then floods a wide red
> glow; a blown white region whites out; the two are the same mechanism.

Concretely for the strategy registry: start from the legacy composite's
additive energy (strength/radius/threshold as shipped), and replace its final
per-channel clamp with (a) hue-ratio-preserving limiting at the pixel, plus
(b) rerouting of the limited overflow into the wide bracket's spatial lobe so
total emitted energy is conserved. The two-component ocular model (colored
near-halo + neutral far-veil) maps naturally onto this: through frosted
acrylic the far lobe grows with drive level until it dominates.

---

*Original issue body follows. Where it conflicts with this directive — in
particular "preserve the unbloomed sharp base" as an unconditional goal and
the earlier white-highlight acceptance bullet — this directive wins.*

## Problem

The HDR bloom pipeline is supposed to make an LED panel glow *in its own colour*: a
saturated red LED should bleed red, a saturated blue LED should bleed blue. Naive
additive bloom drives every bright halo toward white, so the current composite
(`chroma-shoulder`, `src/moviemaker/hdr-bloom-strategies.ts`) detects that condition and
suppresses the bloom energy responsible for it. That detector is
`pixelWhiteMergeRisk(raw, bloomed)`:

```glsl
float colorWash = v1Smoothstep(0.72, 0.98, bloomMin)
    * max(rawSaturation - bloomSaturation, 0.0);
float clippedDetail = v1Smoothstep(0.82, 0.995, bloomMax)
    * v1Smoothstep(0.03, 0.22, bloomMax - rawMax)
    * v1Smoothstep(0.30, 0.85, rawMax);
return max(colorWash, clippedDetail * 0.65);
```

Two failure modes fall out of this design.

**(a) Colored halos still whiten.** Suppression is a subtractive patch applied *after*
the whitening energy has been produced. Where the detector under-fires (mid purity,
mid brightness) a saturated highlight still converges toward white; where it over-fires
the halo is simply missing. There is no setting of the smoothstep edges that separates
"this is a red LED blooming red" from "this is a red LED becoming white" on a per-pixel
scalar.

**(b) Genuinely white highlights lose their bloom entirely — false-positive
suppression.** For a white or near-white source pixel, `rawSaturation ≈ 0`, so
`colorWash` self-cancels — but `clippedDetail` does not. All three of its smoothsteps
fire on a bright achromatic pixel (`bloomMax` high, `bloomMax - rawMax` positive,
`rawMax` in the upper band), risk saturates, and `highWeight`/`midWeight` collapse to
near zero. Downstream, the neutral/chroma split makes it worse: chroma is sourced from
`high - raw`, which on a white core is itself achromatic, so `chromaticBloom → 0`; and
`protectedNeutral`, the *only* bloom a white pixel can have, is what the neutral limits
throttle. The result is a dead white highlight with no glow at all — while the
suppression bought nothing, because a pixel with no hue has no hue to protect.

The discriminator we actually need:

| case | source | bloom behaviour | wanted |
|---|---|---|---|
| (a) | saturated | converging to white | suppress the whitening component |
| (b) | achromatic | blooming white | full glow, no suppression |

This issue surveys what the literature says the right mechanism is, and proposes ranked
experiments implementable as new entries in the strategy registry (selectable in
production via the `bloomStrategy` parameter, `src/production/contract.ts`).

---

## What the literature says

### 1. Physically-based glare is chromatic by construction

The glare literature does not have a "whitening" step to suppress, because the scatter
kernel itself carries the source's spectrum.
[Spencer, Shirley, Zimmerman & Greenberg, "Physically-Based Glare Effects for Digital
Images" (SIGGRAPH '95)](https://www.graphics.cornell.edu/pubs/1995/SSZG95.pdf) decomposes
glare into corneal/lens/retinal scattering plus lens diffraction, and computes the PSF
per wavelength — halos carry fringing rather than flattening to neutral.
[Kakimoto et al., "Glare Generation Based on Wave Optics"
(PG 2005)](http://nishitalab.org/user/nis/cdrom/pg/glare_m.pdf) makes the wavelength
dependence explicit via Fraunhofer diffraction of the aperture;
[Ritschel et al., "Temporal Glare" (CGF 28(2),
2009)](http://people.compute.dtu.dk/jerf/papers/TemporalGlare.pdf) keeps wavelength-
dependent intraocular scattering in a real-time GPU eye model; and
[Hullin et al., "Physically-Based Real-Time Lens Flare Rendering" (SIGGRAPH
2011)](https://resources.mpi-inf.mpg.de/lensflareRendering/) gets hue separation out of
dispersion rather than adding it back afterwards.
[Talvala et al., "Veiling Glare in HDR Imaging" (SIGGRAPH
2007)](https://graphics.stanford.edu/papers/glare_removal/glare_removal.pdf) and
[McCann & Rizzi (JSID 2007)](https://mcimg.us/Retinex/Publications_files/07EI%206492-41.pdf)
frame glare as an additive convolution of the scene's *own* radiance: glare inherits
scene chromaticity everywhere it lands. The correct mental model is **"colored energy
spread"**, not "brightness spread, recoloured".

### 2. Real-time engine practice: the bloom chain is already hue-preserving; the *tonemapper* whitens

[Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH
2014)](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/)
and its reference write-up [LearnOpenGL, "Physically Based Bloom"
(2022)](https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom) run a pyramidal
13-tap down / 9-tap tent-up chain over the *whole* linear HDR buffer with **no brightness
threshold**, taming fireflies with a `1/(1+luma)` weight rather than a cutoff. Identical
filter weights per channel at every mip means hue ratios survive the chain intact.
Where thresholding is kept, the discipline is a *scalar* soft-knee applied to the whole
RGB triplet ([Catlike Coding, Unity SRP
"Bloom"](https://catlikecoding.com/unity/tutorials/advanced-rendering/bloom/);
[Godot glow shaders](https://godotshaders.com/shader/bloom-post-processing-for-viewports/))
— per-channel clipped thresholds are a known source of hue-shift bugs
([Bevy discussion #6655](https://github.com/bevyengine/bevy/discussions/6655)).
[Froyok's UE4 custom bloom](https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/)
composites via `lerp()` rather than pure `+=`, because additive stacking onto already-hot
channels is itself a whitening operation.

Crucially, practitioners consistently locate the whitening *downstream*:
[Epic's filmic/ACES tonemapper docs](https://dev.epicgames.com/documentation/unreal-engine/color-grading-and-the-filmic-tonemapper-in-unreal-engine)
document desaturation-toward-white in the highlight roll-off, and the RealTimeVFX
diagnosis of ["high intensity material going
white"](https://realtimevfx.com/t/high-intensity-material-going-white/24152) is
explicitly "the tonemapper blows emissives to white, not the blur". A companion thread
notes halo and core colour can legitimately *diverge* — a saturated halo around a
tonemapped-white core is a valid compositing choice, not a bug
([RealTimeVFX](https://realtimevfx.com/t/desaturated-washed-out-colours-in-unreal/17798)).
Underpinning all of it: [Lagarde & de Rousiers, "Moving Frostbite to PBR" (SIGGRAPH
2014)](https://seblagarde.wordpress.com/2015/07/14/siggraph-2014-moving-frostbite-to-physically-based-rendering/)
— convert once, composite in linear, encode once.

### 3. Hue-preserving display transforms: compress a norm, hold hue invariant, make desaturation a *parameter*

This is the strongest and most convergent body of work for our problem.

- [Reinhard et al., "Photographic Tone Reproduction" (SIGGRAPH
  2002)](https://www.cs.utah.edu/docs/techreports/2002/pdf/UUCS-02-001.pdf) is the
  origin point: map luminance only (RGB→xyY→RGB) and hue/saturation survive; apply the
  same curve per channel and bright regions desaturate to white.
- [Sobotka's AgX](https://github.com/sobotka) / [AgXc](https://github.com/MrLixm/AgXc)
  names the failure — the "Notorious Six": per-channel transforms rotate colours toward
  whichever of the six RGB/CMY axes clips first. AgX rotates primaries *before* the
  curve so the path-to-white is engineered rather than accidental; it shipped as
  [Blender 4.0's default view
  transform](https://developer.blender.org/docs/release_notes/4.0/color_management/)
  precisely because Filmic's per-channel rolloff produced hue shifts in bright saturated
  areas (fire, LEDs). It exposes a **"preserve hue" slider** — a blend between hue
  fidelity and artistic path-to-white
  ([Avid Andrew's walkthrough](https://avidandrew.com/agx-color.html)).
- [ACES 2.0 chroma
  compression](https://docs.acescentral.com/system-components/output-transforms/technical-details/chroma-compression/)
  works in Hellwig-2022 **JMh**: compression acts only on colorfulness **M**, with
  lightness **J** and hue **h** held exactly constant, and — the key property for us —
  **compression strength increases with J (brightness) and decreases with M (existing
  saturation)**. Already-saturated colours resist compression; near-white highlights
  compress hardest. [ACES 1.3 Reference Gamut
  Compression](https://docs.acescentral.com/rgc/specification/) does the analogous
  "heal" for out-of-gamut saturated bright sources — police lights, stoplights, LEDs —
  via a smooth radial function centred on hue.
- [Jed Smith's OpenDRT](https://github.com/jedypod/open-display-transform) tonemaps a
  single **max-RGB norm** and re-derives RGB by ratio, then applies an explicit
  "dechroma" purity compression as values approach display max — avoiding per-channel
  clipping *by construction*
  ([discussion](https://github.com/jedypod/open-display-transform/discussions/41)).
- [Ottosson, "sRGB gamut clipping"
  (2021)](https://bottosson.github.io/posts/gamutclipping/) gives five Oklab strategies,
  all hue-locked, projecting only in the lightness–chroma plane, with an adaptive blend
  parameter α — runnable MIT-licensed reference code.
- [Khronos PBR Neutral](https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md)
  ([paper](https://dl.acm.org/doi/fullHtml/10.1145/3641233.3664313)) is ~13 lines of
  GLSL: hue-invariant by construction, 1:1 up to a threshold, then controlled
  desaturation along a single path.
- Broadcast agrees. [ITU-R BT.2408/BT.2390](https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2408-8-2024-PDF-E.pdf)
  compresses **I** in ICtCp leaving Ct/Cp comparatively untouched, explicitly to avoid
  desaturation from non-uniform channel scaling;
  [BT.2446](https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2446-1-2021-PDF-E.pdf)
  adds an explicit chroma-correction scale derived from the input/output luminance
  ratio — and notes that driving over-range chroma achromatic is a *deliberate,
  separately controlled* choice.
  [Frostbite's HDR grading course (SIGGRAPH
  2017)](https://www.slideshare.net/DICEStudio/high-dynamic-range-color-grading-and-display-in-frostbite)
  does the same in ICtCp with an artist-tunable "crosstalk" parameter.

Two dissenting/nuancing voices worth keeping: [Hable's Uncharted 2 filmic
curve](https://filmicworlds.com/) and the [Gran Turismo
tonemap](https://www.shadertoy.com/view/Xstyzn) treat shoulder desaturation as a
*desirable*, deliberately shaped design choice — path-to-white is not automatically a
defect, it just has to be intentional and tunable.

### 4. Perception: brighter should look *more* colourful, and glare has two components

The [Hunt effect](https://ies.org/definitions/hunt-effect/) (Hunt 1952, formalized in
CIECAM02/[CAM16](https://en.wikipedia.org/wiki/CAM16)) says perceived colorfulness
*increases* with luminance at fixed chromaticity. Pipelines that desaturate highlights
are fighting the expected perceptual response.
[Mantiuk et al., "Color Correction for Tone Mapping" (EG
2009)](https://www.cl.cam.ac.uk/~rkm38/pdfs/mantiuk09cctm.pdf) gives the corrective
pattern: apply the compression, then reboost chroma by a per-pixel factor derived from
the *local contrast-compression ratio*.

On the spatial side, the ocular PSF decomposes into three zones — a sharp central peak,
a mid-radius halo lobe, and an outer near-uniform diffuse veil — where **only the inner
halo lobe tracks the source's chromaticity, while the outer veil is effectively neutral**
([review](https://www.researchgate.net/publication/47385003);
[Coppens et al. on λ⁻⁴ straylight](https://pubmed.ncbi.nlm.nih.gov/16293245/);
[Vos & van den Berg, CIE disability
glare](https://journals.sagepub.com/doi/10.1191/1477153503li083oa), which finds total
foveal glare *energy* roughly wavelength-neutral while its spatial/spectral distribution
is not). This is a concrete **two-component model: colored near-halo + neutral far-veil**
— and it legitimizes neutral bloom. A white core's neutral veil is *physically correct*,
which is directly the literature's answer to failure mode (b).

---

## Analysis: mapping this onto our pipeline

### The structural claim

Our approach is **produce whitening, then detect and subtract it**. The literature
consensus is **never produce the whitening in the first place**:

1. Keep the bloom convolution hue-preserving (§1, §2) — same weights per channel, linear
   float buffers, scalar (not per-channel) gating.
2. Do brightness compression on a *single axis* — a norm, luminance, or lightness — with
   hue held literally invariant (§3).
3. Make desaturation an **explicit tunable parameter** ("path to white": AgX preserve-hue,
   Ottosson's α, Frostbite crosstalk, OpenDRT dechroma), driven by brightness *and*
   existing saturation, rather than an emergent artifact you then have to detect.

Under (2)+(3) there *is no suppression heuristic* that can false-positive on white
pixels, because there is no whitening event to detect. `pixelWhiteMergeRisk` is a patch
on a structural problem.

Two more specific observations about the current code:

- **What we already do right.** Every strategy ends with a uniform `hueSafeScale` — one
  scalar min-ratio against per-channel headroom, applied to the whole added vector — and
  a single `linearToSrgb` encode. That *is* the literature's hue-preserving gamut
  projection (Ottosson-style, OpenDRT-style), and it is why we don't see Notorious-Six
  hue rotation at the final clamp. The whitening we do see comes from *before* it: the
  `protectedNeutral` term adds equal energy to all three channels, which is a
  saturation-reducing operation by definition. That is the whitening event.
- **A mixed-space wart.** `pixelWhiteMergeRisk` is fed `linearToSrgb(...)`-encoded values
  while everything it modulates is composited in linear. Its smoothstep edges therefore
  live in a different space from the energy they gate. Single-axis (norm / J) approaches
  avoid this entirely by never needing a second space.

### Where each existing strategy sits

| strategy | literature position |
|---|---|
| `chroma-shoulder` (default) | The detect-and-subtract anti-pattern. Correct in spirit (separate neutral from chromatic energy; uniform-vector shoulder; single hue-safe scale) but the gate is a per-pixel scalar heuristic in the wrong colour space, and it has no term expressing "source is achromatic → nothing to protect". |
| `linear-hdr` | Same detector, plus bloom *replaces* the base (`mix(rawLinear, bloomComposite, …)`) rather than adding to it — violates the CLAUDE.md rule that bloom is added light over a preserved sharp base, and needed a 0.30 global scale to stay controlled. |
| `chroma-capped` | Caps neutral + chromatic energy jointly against one ceiling — a step toward uniform scaling (§2's scalar discipline), and the reason it holds black best. Still gated by the same v1 detector. |
| `white-core-chroma` | The closest existing analogue of **ACES 2.0's M-dependence**: it multiplies `clippedDetail` by `v1Smoothstep(0.04, 0.18, rawSaturation)`, i.e. lets source chroma modulate how much suppression is permitted. This is the RGB-space approximation of "compression strength decreases with M". It also decouples hue *direction* (most-chromatic bracket) from *energy* (selected bracket). |
| `sliding-window` | The closest to modern engine consensus (§2): threshold-free continuous window positioned by per-pixel headroom, one interpolation, one uniform-vector exponential shoulder on the whole added colour, no neutral/chroma split, no risk heuristic at all. It is the existing strategy that *cannot* false-positive on white pixels. |
| `wide-surround-chroma` | An implementation of the ocular **two-component model** (§4): `radiusScales: [1, 1, 3.2]` gives the brackets real spatial separation, and the annulus `wide - narrow` isolates surround light that still carries hue when the core has none. |

### The white-pixel discriminator, mechanism by mechanism

Which literature mechanisms actually solve (b) *structurally*, rather than by retuning
smoothstep edges:

1. **Raw-saturation (M) gating** — ACES 2.0 JMh chroma compression decreases with
   existing M; Mantiuk's correction factor is per-pixel. Structurally, "how much may I
   desaturate this pixel" must be a function of the pixel's *source* chroma. A pixel with
   M ≈ 0 should be exempt from all chroma protection. `white-core-chroma` already does
   this in RGB.
2. **Surround-hue reference** — the two-component ocular model plus Talvala/McCann's
   "glare inherits scene chromaticity" says a blown core's halo should carry the
   *surround's* hue. This solves (b) by making the white core's near-halo chromatic where
   the scene has colour nearby, and neutral where it genuinely doesn't.
   `wide-surround-chroma` is this.
3. **Norm-based tonemapping** — OpenDRT / Khronos PBR Neutral / Reinhard-luminance /
   BT.2408 ICtCp: compress one achromatic axis, re-derive RGB by ratio. No suppression
   heuristic exists, so none can misfire. A white pixel and a red pixel take the same
   code path; the white pixel simply has zero chroma to compress.
4. **Post-compression chroma restoration** (Mantiuk) — let the composite compress
   normally, then reboost chroma proportionally to how much local contrast was flattened.
   On a white pixel the reboost factor multiplies zero chroma, so it is automatically a
   no-op: *the discriminator is free*.
5. **Hunt-effect chroma boost** (CIECAM02/CAM16) — the perceptual argument that bright
   halos should gain, not lose, colorfulness. Also self-nulling on achromatic pixels.
6. **Legitimizing neutral bloom** — per §4, the far-veil is *supposed* to be neutral. A
   white highlight blooming with a neutral veil is physically right; our bug is that
   `protectedNeutral` is throttled hardest exactly there. Any candidate must let white
   pixels keep neutral energy while still preventing neutral energy from washing
   *coloured* pixels.

---

## Candidate directions

Each is implementable as a new entry in `HDR_BLOOM_STRATEGIES`
(`src/moviemaker/hdr-bloom-strategies.ts`), selectable via `bloomStrategy=<name>` in a
production job URL. Ranked by expected structural payoff, not by confidence.

### 1. `norm-tonescale` — compress a max-RGB norm, re-derive RGB by ratio

**Mechanism.** Add bloom in linear as usual (`added = selected - raw`, no risk gating at
all). Form `total = raw + added`. Compute `n = maxChannel(total)`; tonemap `n` through a
single shoulder (Khronos PBR Neutral's ~13 lines, or the existing exponential shoulder);
set `out = total * (toneMap(n) / n)`. Then apply a *separate*, explicit dechroma:
`out = mix(out, vec3(norm), pathToWhite)` where
`pathToWhite = smoothstep(a, b, J) * (1 - smoothstep(c, d, M))` — increasing with
brightness, decreasing with source saturation.

**Sources.** OpenDRT; Khronos PBR Neutral; Reinhard 2002; ACES 2.0 chroma compression
(the J↑/M↓ dependence); Jimenez / LearnOpenGL for the threshold-free bloom feeding it.

**Expected effect.** (i) Saturated highlights keep hue exactly — ratio-preservation is
algebraic, not heuristic. (ii) White highlights bloom fully; `pathToWhite` multiplies zero
chroma, and no detector exists to misfire. (iii) Shadows: the norm shoulder is
near-identity at low values, so black should hold — but the removal of `darkMask` /
`neutralLimit` needs care.

**Risk.** Highest of any candidate: it changes *every* pixel on screen, not just the
failure cases. It is the largest regression surface against the #49 (bloom becoming
imperceptible on dense maps) and #53 (halos washing the panel) precedents, and against
the "deep shadows stay black" requirement. Expect to re-derive the dark-mask behaviour
from scratch.

### 2. `path-to-white` — explicit tunable desaturation in a polar space

**Mechanism.** Keep the current bracket capture and the added-light structure, but
replace `pixelWhiteMergeRisk` + `protectedNeutral` throttling with: convert
`raw + added` to Oklab, hold `h` fixed, apply the tonescale on `L`, and compress `C` by a
factor `f(L, C_source)` — more compression as `L` rises, less as `C_source` rises.
Expose the blend as a single strategy-level constant (AgX's preserve-hue slider,
Ottosson's α).

**Sources.** Ottosson gamut clipping (runnable Oklab reference); AgX / Blender 4.0;
ACES 2.0 JMh; Frostbite ICtCp crosstalk; BT.2408.

**Expected effect.** (i) Hue literally invariant; colour survives to display max.
(ii) `C_source = 0` → the compression term is a no-op; white pixels get the full
tonescale and full glow. (iii) Neutral for shadows (identity at low L), which makes this
the *lower-risk* sibling of candidate 1.

**Risk.** Oklab round-trip cost per pixel at 1024×1024 (probably negligible, but measure
against the #255/#256 frame-cadence precedents). Oklab's blue-hue non-constancy at
extreme chroma can look wrong on saturated blue LEDs — worth checking specifically.

### 3. `two-component-veil` — formalize colored near-halo + neutral far-veil

**Mechanism.** Give the brackets explicit spatial roles via `radiusScales` (e.g.
`[1, 2, 6]`): the narrow/mid brackets are the **chromatic near-halo**, composited as a
hue-preserving vector; the wide bracket is the **neutral far-veil**, reduced to its
luminance and added at low, globally capped strength. Where the near-halo has no chroma
(white core), the near-halo term is still added neutrally — no gate.

**Sources.** Ocular PSF three-zone review; Coppens λ⁻⁴ straylight; Vos & van den Berg;
Talvala 2007; McCann & Rizzi; Epic's Convolution/FFT bloom as the energy-conserving
scatter analogue.

**Expected effect.** (i) Colored LEDs get a coloured near-halo (this is what the near
component *is*). (ii) White LEDs get a full neutral veil — physically correct, and the
literature's direct answer to (b). (iii) The far-veil is the term most likely to lift
blacks; it must be tightly capped, which is exactly the Talvala veiling-glare warning.

**Risk.** Two extra blur radii at 1024×1024 is real GPU cost. The far-veil directly
threatens the "no scene-wide veil" requirement. Interacts with #491 (topology-aware local
density): wide radii behave very differently on 16×16 vs 64×64 maps.

### 4. `chroma-restore` — minimal Mantiuk-style post-compression reboost

**Mechanism.** Smallest diff of the five. Leave the current composite intact, but delete
the `clippedDetail` risk term's ability to fire on achromatic pixels (as
`white-core-chroma` does) and append a final stage: measure the saturation ratio
`s_out / s_raw` on the composited pixel, and scale the chroma component back up by a
capped factor derived from it.

**Sources.** Mantiuk et al. 2009; Hunt effect / CAM16; BT.2446's luminance-ratio chroma
correction.

**Expected effect.** (i) Recovers hue that the composite flattened, without needing to
predict it in advance. (ii) Automatically a no-op on white pixels (the factor multiplies
zero chroma) — the discriminator comes for free. (iii) Should not touch shadows if gated
on absolute output level.

**Risk.** Still a corrective patch layered on the existing structure, so it inherits
`chroma-shoulder`'s other weaknesses. Reboost can amplify chroma noise in dim,
low-purity regions; needs a hard cap and a low-level gate.

### 5. `hunt-boost` — brightness-proportional colorfulness gain

**Mechanism.** Cheapest experiment; mostly a control. Scale the chromatic bloom vector by
`1 + k·smoothstep(lo, hi, luminance)` so bright halos gain colorfulness with brightness
instead of losing it. Everything else unchanged from `white-core-chroma`.

**Sources.** Hunt effect; CAM16; ACES 2.0's J-dependence.

**Expected effect.** (i) Visibly more colourful saturated halos. (ii) Self-nulling on
white pixels (zero chroma × any gain = zero), so it neither helps nor hurts (b) — pair
it with `white-core-chroma`'s gate to address (b). (iii) No shadow effect if gated on
luminance.

**Risk.** Can overshoot into gamut clipping, which the final `hueSafeScale` will then
scale back — potentially producing brightness-dependent halo dimming. Purely additive
value; not a candidate for default on its own.

---

## Evaluation protocol

Follow the CLAUDE.md "HDR-bloom tuning protocol" without deviation. Each candidate is a
new registry entry, so every render is reproducible by name via `bloomStrategy=<name>`.

- Fixed source / map / fps per experiment. Render baseline (`chroma-shoulder`) and
  candidate at identical 1024×1024 geometry, `videoMode=mapped-led`, same `outputFps`.
- Build the horizontal splice with ffmpeg, **baseline left, candidate right**; keep both
  inputs and the splice in `E:\video\short_out` with stable versioned names.
- Judge at known timestamps, not from one still. Mandatory checks: **frame 0** (temporal
  exposure must be primed/settled), bright highlights, skin/midtones, deep shadows.
- **Required test scenes — both, every time:**
  1. A **pure-white / near-white highlight** case (specular, white LED, blown source
     region). Pass condition: the highlight visibly glows. This is the regression that
     motivated the issue.
  2. A **saturated-colour** case (strong red and strong blue sources). Pass condition:
     the halo stays in the source hue out to the edge of the bleed, with no white core
     ring.
- Guard the precedents: #49 (bloom must not become imperceptible on dense maps), #53
  (large diameters must not wash the panel), #55 (geometry-aware iris), #255/#256
  (frame cadence and verified output FPS must be preserved). Test at least one sparse
  (16×16) and one dense (64×64) map; see also #491 for topology-aware local density.
- Record the exact job URL parameters, map, source filename, strategy name, and
  candidate-vs-baseline observations alongside each render.

No candidate should be promoted to default on reasoning alone — the A/B splice is the
arbiter.

### Honest uncertainty

- Candidates 1 and 2 are structural rewrites of the tone response; they may well look
  *worse* on first render than the heavily hand-tuned `chroma-shoulder`, and would need
  their own tuning pass before a fair comparison. Do not read a bad first splice as a
  refutation of the approach.
- The literature is nearly all about *display transforms for photographic/rendered
  scenes*. An LED panel is an emissive, sparse, high-contrast point-source layout with
  black gaps — the regime where "deep shadows stay black" is a hard constraint that most
  of these transforms were not designed against.
- No source here measures a simulated LED panel specifically. The mapping from
  display-transform practice to bloom compositing on this content is an inference, not a
  cited result.

---

## References

**Glare, optics, perception**
1. Spencer, Shirley, Zimmerman, Greenberg — "Physically-Based Glare Effects for Digital Images", SIGGRAPH '95 — https://www.graphics.cornell.edu/pubs/1995/SSZG95.pdf
2. Kakimoto, Matsuoka, Nishita, Naemura, Harashima — "Glare Generation Based on Wave Optics", PG/CGF 24(2), 2005 — http://nishitalab.org/user/nis/cdrom/pg/glare_m.pdf
3. Kakimoto et al. — "Glare Simulation ... Spectral Power Distribution", SIGGRAPH 2005 Posters — https://dl.acm.org/doi/10.1145/1186954.1187003
4. Ritschel, Ihrke, Frisvad, Coppens, Myszkowski, Seidel — "Temporal Glare", CGF 28(2), 2009 — http://people.compute.dtu.dk/jerf/papers/TemporalGlare.pdf
5. Hullin, Eisemann, Seidel, Lee — "Physically-Based Real-Time Lens Flare Rendering", SIGGRAPH 2011 — https://resources.mpi-inf.mpg.de/lensflareRendering/
6. Talvala, Adams, Horowitz, Levoy — "Veiling Glare in High Dynamic Range Imaging", SIGGRAPH 2007 — https://graphics.stanford.edu/papers/glare_removal/glare_removal.pdf
7. McCann & Rizzi — "Camera and Visual Veiling Glare in HDR Images", JSID 2007 — https://mcimg.us/Retinex/Publications_files/07EI%206492-41.pdf
8. Coppens, Franssen, van den Berg — "Wavelength Dependence of Intraocular Straylight", Exp. Eye Res. 2006 — https://pubmed.ncbi.nlm.nih.gov/16293245/
9. Vos & van den Berg — CIE disability-glare equation, "Reflections on Glare", LR&T 2003 — https://journals.sagepub.com/doi/10.1191/1477153503li083oa
10. "Intraocular Light Scattering in Vision, Artistic Painting, and Photography" (review) — https://www.researchgate.net/publication/47385003
11. Hunt effect — https://ies.org/definitions/hunt-effect/ · CAM16 — https://en.wikipedia.org/wiki/CAM16

**Tone mapping & display transforms**

12. Reinhard, Stark, Shirley, Ferwerda — "Photographic Tone Reproduction for Digital Images", SIGGRAPH 2002 — https://www.cs.utah.edu/docs/techreports/2002/pdf/UUCS-02-001.pdf
13. Mantiuk, Mantiuk, Tomaszewska, Heidrich — "Color Correction for Tone Mapping", Eurographics 2009 — https://www.cl.cam.ac.uk/~rkm38/pdfs/mantiuk09cctm.pdf
14. Sobotka — AgX — https://github.com/sobotka · AgXc — https://github.com/MrLixm/AgXc
15. Blender 4.0 color management release notes (AgX adoption) — https://developer.blender.org/docs/release_notes/4.0/color_management/
16. Avid Andrew — "AgX, Color Shifts, and the Notorious 6" — https://avidandrew.com/agx-color.html
17. ACES 1.3 Reference Gamut Compression — https://docs.acescentral.com/rgc/specification/
18. ACES 2.0 Output Transforms, chroma compression — https://docs.acescentral.com/system-components/output-transforms/technical-details/chroma-compression/
19. Jed Smith — OpenDRT — https://github.com/jedypod/open-display-transform · https://github.com/jedypod/open-display-transform/discussions/41
20. Ottosson — "sRGB gamut clipping" (Oklab) — https://bottosson.github.io/posts/gamutclipping/
21. Khronos PBR Neutral Tone Mapper — https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md · https://dl.acm.org/doi/fullHtml/10.1145/3641233.3664313
22. Frostbite — "HDR color grading and display in Frostbite", SIGGRAPH 2017 — https://www.slideshare.net/DICEStudio/high-dynamic-range-color-grading-and-display-in-frostbite
23. Uchimura — Gran Turismo tonemap, CEDEC 2017 — https://www.shadertoy.com/view/Xstyzn
24. Hable — Uncharted 2 HDR lighting, GDC 2010 — https://filmicworlds.com/
25. ITU-R BT.2408 (2024) / BT.2390 — https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2408-8-2024-PDF-E.pdf
26. ITU-R BT.2446 (2021) — https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2446-1-2021-PDF-E.pdf

**Real-time bloom practice**

27. Jimenez — "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 — https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
28. LearnOpenGL — "Physically Based Bloom" (2022) — https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom
29. Froyok (Léna Piquet) — "Custom Bloom Post-Process in Unreal Engine" (2021) — https://www.froyok.fr/blog/2021-12-ue4-custom-bloom/
30. Epic — "Bloom in Unreal Engine" (Standard vs Convolution/FFT) — https://dev.epicgames.com/documentation/en-us/unreal-engine/bloom-in-unreal-engine
31. Epic — "Color Grading and the Filmic Tonemapper" — https://dev.epicgames.com/documentation/unreal-engine/color-grading-and-the-filmic-tonemapper-in-unreal-engine
32. Catlike Coding — "Bloom" (Unity SRP) — https://catlikecoding.com/unity/tutorials/advanced-rendering/bloom/
33. Unity HDRP Bloom manual — https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.1/manual/Post-Processing-Bloom.html
34. Godot bloom/glow community shader — https://godotshaders.com/shader/bloom-post-processing-for-viewports/
35. Lagarde & de Rousiers — "Moving Frostbite to PBR", SIGGRAPH 2014 — https://seblagarde.wordpress.com/2015/07/14/siggraph-2014-moving-frostbite-to-physically-based-rendering/
36. Bevy Engine discussion #6655 (threshold-free linear bloom) — https://github.com/bevyengine/bevy/discussions/6655
37. Kawase — "Frame Buffer Postprocessing Effects in DOUBLE-S.T.E.A.L", GDC 2003 — https://www.chrisoat.com/papers/Oat-ScenePostprocessing.pdf
38. NVIDIA GPU Gems Ch. 21, "Real-Time Glow" — https://developer.nvidia.com/gpugems/gpugems/part-iv-image-processing/chapter-21-real-time-glow
39. RealTimeVFX — "High intensity material going white" — https://realtimevfx.com/t/high-intensity-material-going-white/24152
40. RealTimeVFX — "Desaturated/washed out colours in Unreal" — https://realtimevfx.com/t/desaturated-washed-out-colours-in-unreal/17798

