# Texture and render-target formats (kh2fm.exe + OpenKH data)

Machine-readable: `formats.json`.

## Confirmed from the exe (DDS loader fn 0x118a90, texture creation fn 0x118710)
- **Shipping GPU texture formats: BC1 (0x47), BC2 (0x4a), BC3 (0x4d), B8G8R8A8_UNORM (0x57)**, plus NV12 (0x67, movie only) and R8G8_UNORM (0x31, movie UV). No DX10 header handling, so **no BC4/BC5/BC6H/BC7** ever.
- The loader recognises A4R4G4B4, A1R5G5B5, R5G6B5 and 24-bit R8G8B8 masks but **jumps to the function epilogue (0x118fd9) without creating a texture**: these formats are unsupported by the engine (corrects the gate note that their conversion path was untraced - there is none). Any other pixel format leaves format 0 (CreateTexture would fail).
- PNG (libpng) and raw/blank textures use 0x57. Mip count capped at 8.

## Confirmed from the OpenKH extracted data (read-only scan on build-host, 37,757 DDS)
| Format | Files | Share |
|---|---|---|
| FOURCC DXT5 (BC3) | 26,865 | 71.2% |
| A8R8G8B8 (BGRA8) | 10,892 | 28.8% |
| anything else (DXT1/3, DX10, 16-bit, 24-bit, cubemap, volume) | 0 | 0% |

Sizes: 1024x1024 (7,180), 512x512 (6,126), 1024x512 (5,517), 256x256 (4,576), 256x128 (3,013) dominate. 5,733 PNG files exist in the same tree. This agrees with the plan's "about 1/3 DXT5, 2/3 uncompressed" only in direction: in this full census it is 71% DXT5 / 29% uncompressed (the earlier figure came from a 400-file sample).
Caveat: the SillyBillyMod1 data overlay was not counted separately; if it adds other pixel formats they would be dropped by the loader as above.

## Render targets (static)
- Swapchain B8G8R8A8_UNORM (3 buffers); every PSO has exactly one RTV format, 0x57, and DSV 0x14 D32_FLOAT_S8X24_UINT (see PSO-KEY.md). GX surface objects default to 0x57 (colour) / 0x14 (depth) (fns 0x11ba4f, 0x11cd46, 0x11be5f).
- MSAA 1/2/4/8 picked at startup by CheckFeatureSupport on B8G8R8A8; explicit resolve (BGRA8).
- **Only runtime capture can settle**: each GX surface's width/height, which surfaces are multisampled, RT-set record count, movie frame size.

## Metal mapping
See the table in `formats.json` (`metal_mapping`). Key points: BGRA8 maps 1:1 with no swizzle; BC1-3 exist in Metal but iPhone-GPU support is unverified, so the plan's offline ASTC transcode stays (BC3 -> ASTC 4x4 preserves the 4x4 block grid the game's footprint maths assumes; larger ASTC blocks need the shim to rewrite `GetCopyableFootprints`); depth-stencil -> Depth32Float_Stencil8; NV12 -> two plane views; the unused packed 16-bit formats map to ABGR4Unorm / BGR5A1Unorm / B5G6R5Unorm (component order needs a swizzle), 24-bit has no Metal format.
