# Yamaha ESP sample and waveform observations

Read-only inspection, 2026-10-29, of locally installed ESP **3.0.1 build 427**. These
observations inform [Oscairn's sample milestone](sample-players-and-formats.md); no Yamaha
implementation or sample content is incorporated into Oscairn.

## What Yamaha exposes to musicians

The [MONTAGE M Waveform Edit documentation](https://manual.yamaha.com/mi/synth/montage_m/en/om02screenparameters0090.html)
describes a Waveform assembled from Key Banks, with note/velocity ranges, a center note,
level, pan and tuning. An AWM2 Element chooses a Waveform. This is a useful distinction:
audio content, mapping and the sound's synthesis settings are separate concepts.

That source describes the hardware UI, not proof of every ESP feature. ESP's installed
symbols independently contain `WaveformEditView`, `KeybankMapGraph`,
`ContSrv::WaveformData::{addKeybank,getKeybank,setKeybank}` and
`VsyMaisyWaveformDataCreator`. They support the presence of corresponding concepts in
the desktop implementation. No interactive ESP editing/session-export test was performed.

## Reproducibility

Inspected the arm64 slice of
`/Library/Audio/Plug-Ins/VST3/Yamaha/Expanded Softsynth Plugin for MONTAGE M.vst3/Contents/MacOS/Expanded Softsynth Plugin for MONTAGE M`.
The universal executable is 136,163,984 bytes, SHA-256
`2d54e3d3617ff74a7421436b7a8374938f6922ed6d2810f7051af7b0e011ffb4`.
Used `nm -arch arm64`, `c++filt` and Xcode `llvm-objdump`; addresses below are unslid
arm64 virtual addresses. Only findings/symbol references are stored in the repository.

## Direct observations and their limits

| Evidence | Established observation | What it does not prove |
| --- | --- | --- |
| `Contents/Resources/contents/waveform.cfg` | A 353,999-byte binary catalog with `PChd`/`Entr` records and readable waveform labels. No loose WAV files were found in this specific contents tree. | It is not evidence of the location, encoding, total size or ownership of all factory PCM. No library extraction was attempted. |
| `VsyMaisyWaveformDataCreator::setSampleData(bool,void*,int)`, `0xae7930` | Takes a recursive mutex and dispatches to `convertWave<float>` or `convertWave<double>`. `setSampleRate`, `0xae78c4`, is a separate operation. | This does not identify the thread on which every call executes, all import formats, or the complete conversion algorithm. |
| `ysyn::SoftMontage::CSawmtgSvtg::SetWaveCache`, `0x6f006c` | Selects one of two stored server/info pairs for selector 0 or 1 and forwards to `ysyn::SoftAWM2::SetWxcWaveCache`. The matching clear wrapper is at `0x6f00ac`. | The two selectors have not been identified as Parts, engines, channels or memory tiers. Do not assign them a musical meaning from this wrapper alone. |
| `ysyn::wxcdec::CData::SetWaveCache(int)`, `0x702f9c` | Takes a mutex, checks completion/progress, calls `DecodeWxcFrame` or `DecodeLinearFrame`, advances stored progress and can free source storage when finished. `SetWaveCacheAll`, `0x703090`, has a related whole-data path. | This is evidence of decoded caching, not proof that ESP streams disk data in its audio callback, shares all PCM between Parts, or follows Oscairn's real-time constraints. |
| `MmsApplication::openWaveformFile`, `0x9daa58`, and `openWaveFile`, `0x9dab34` | Separate named entry points for waveform and wave-file access exist. | Symbol names alone do not establish file layouts or load/save compatibility. |

## What to carry into Oscairn

Keep immutable samples, region mappings, patch settings and playing voices separate.
A musician should be able to reuse one sample or mapped instrument in several sounds
without duplicating the decoded audio merely because filter/envelope settings differ.
An outgoing voice must retain the sample and settings it needs until it ends.

Use ordinary WAV/SFZ/SoundFont content and proven open-source components where appropriate;
do not reproduce Yamaha's proprietary file formats, compressed codec or implementation
from this inspection. The first bounded WAV slice does not need Yamaha's cache design.

This inspection did **not** measure heap residency, CPU, actual memory savings between
Parts/instances, disk-streaming deadlines, hardware sample-ROM behavior, or Yamaha's
seamless-transition guarantees. The ESP v3 manual PDF could not be fetched by the web
tool (HTTP 403); the hardware manual and local binary observations are labeled separately.
No authentication, licensing or encrypted-content protection was bypassed.
