# R6 Forever Water — Accepted Live Authority (2026-10-01)

## Status

R6 is accepted for the Solo Ironman WoW graphics stack as the additive
Forever-derived water companion layered over frozen R0-R5.

## Frozen R0-R5 authority

- Wow.exe
  - SHA256: `57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da`
- d3d9.dll
  - SHA256: `73727ffdb3274eda5c42aaf610fae5f1ff3e0fdc0f5ac7e23fffee9c44bcd2f1`
- WarcraftXL.dll
  - SHA256: `e18b2d9c5daa5bfe17439ea89a12585235d24f373f239e8d8e49a7ef53f69dfd`

These three binaries were re-hashed before and after the R6 compatibility
upgrade and remained unchanged.

## Accepted R6 companion authority

Built from branch:
`gfx-r6-forever-water-r0r5-compat-20261001`

Source head before this acceptance record:
`01aab93e5446261ea6cdf4eb875ec7c82be54b15`

Pinned upstream CoAVolFog:
`4e31ddf53a326ba9c0ad03488b4c30f7ce23c969`

Installed companion hashes:

- version.dll
  - `cfe5d1a7f535128611ea85288e67abc34e571db6968849343303c0429efd5ad1`
- CoAVolFog.dll
  - `b1e3c52193d1ad4bb816793dae90dcc0dd20376f9974c64613f7821737534eda`
- CoAVolFog.ini
  - `7e9357ec4f25611394c61804fdb6b0908835390aa8d5f9f12eeb07f7dc43d00b`
- fogdata.bin
  - `4276f4b8511ae78c854faebaea7931ff9c57439179623ad7ffd64284e78d160a`
- waterdata.bin
  - `6e09ad128d0d73c94231e76b4491a8bbac1a27fad9c1aaa54c846f04ad72d99c`

Candidate ZIP SHA256:
`2cdc9e358a7805027b28101e1a41a99c6db212956673afa8146b7bba3c16faa2`

## Locked water tuning

`WaterReflections=0.75`

Other R6 water settings remain the qualified water-only profile.

## Live Proton/DXVK acceptance evidence

Live system:
- NVIDIA GeForce RTX 5060
- 3840x2160
- requested 8x MSAA

Observed live gate:

- `multisampling offered ... WXL two-stage StretchRect`
- `depth copy self-test: WXL two-stage StretchRect copies the 8x 3840x2160 depth into INTZ`
- cleared-depth copy timing: 0.532 ms
- `ms requested 8 ... used 8`
- `multisampling 8x kept`
- lake classification passed
- ocean classification passed
- FFT water waves: 256x256
- water shaded at quality 3
- R6 fog path skipped explicitly: `fog skipped: R6 water-only profile`
- water GPU median: 2.07 ms over 4171 frames, 0 skipped
- client splash/wake sprites restored on shutdown

## R6 acceptance boundaries

R6 proves the production water path under the user's actual Proton/DXVK
environment. It does not claim pixel-exact WoW Forever parity.

The accepted architecture is:
- native client liquid geometry/classification;
- Forever-derived water data;
- FFT waves;
- depth/refraction/absorption;
- foam;
- SSR plus sky fallback;
- specular/glint;
- local ripples/wakes;
- WarcraftXL-compatible 8x-MSAA depth transport.

R7 must remain additive to this authority. In particular:
- do not replace Wow.exe, d3d9.dll, or WarcraftXL.dll;
- do not alter the accepted R6 companion DLLs without a new qualification;
- retain `WaterReflections=0.75`;
- retain the two-stage StretchRect MSAA path;
- keep CoAVolFog `FarClipMax=0` so accepted R4 view-distance policy remains authoritative;
- introduce atmosphere/lighting features in isolated, reversible tranches.
