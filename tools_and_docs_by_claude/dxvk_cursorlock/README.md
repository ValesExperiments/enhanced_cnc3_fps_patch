# DXVK 3.1.1 with cursor lock and borderless-VRR mode (d3d9)

A 32-bit `d3d9.dll` built from upstream DXVK v3.1.1 plus two small changes:

1. **Cursor lock.** In fullscreen, the cursor is confined to the game window
   while it has focus, like native D3D9 exclusive fullscreen. Without this,
   the cursor can leave the game on multi-monitor setups and C&C3's edge
   scrolling stops working.
2. **Borderless VRR mode.** Upstream DXVK tells the Vulkan driver that
   full-screen exclusive is `DISALLOWED`, so the driver only sees a plain
   borderless window. With G-Sync set to "full screen only", NVIDIA then
   never engages G-Sync. This build passes `DEFAULT` instead (what native
   Vulkan games get), letting the driver decide while staying borderless.
   Real exclusive fullscreen (`dxvk.allowFse`) stays off.

## Files

| Path | Tracked? | What |
|---|---|---|
| `cursor_lock.patch` | yes | Both changes, as a diff against upstream `src/d3d9` and `src/dxvk` |
| `build_dxvk.bat` | yes | Builds `out\d3d9.dll` with MSVC (VS 2026, x86) |
| `source\` | no | Working copy of `..\gitignored\dxvk` (v3.1.1) with the patch applied |
| `out\d3d9.dll` | no | Build result |

The reference clone `tools_and_docs_by_claude\gitignored\dxvk` is never
modified. To rebuild from scratch, copy it (without `.git`) to `source\`,
apply `cursor_lock.patch` (`git apply` from inside `source\`), then run
`build_dxvk.bat clean`.

## Behaviour

- New option **`dxvk.fseDriverDefault`** (default `True`). `False` restores
  upstream's `DISALLOWED`. The DXVK log prints the mode in use ("Presenter:
  full-screen exclusive mode ...").
- New option **`d3d9.clipCursor`** (default `True`). To turn it off, put
  `d3d9.clipCursor = False` in a `dxvk.conf` next to the game executable.
- The cursor is clipped only when the swap chain is fullscreen, its window is
  the foreground window, and it's not minimized. It's re-checked on every
  Present, because Windows drops the clip on focus or display changes.
- The clip is released immediately on focus loss (`WM_ACTIVATEAPP`), when
  leaving fullscreen, and when the swap chain is destroyed.

## Build requirements

- Visual Studio 2026 Community (system-wide), x86 toolset
- meson and ninja (in `tools_and_docs_by_claude\venv`)
- glslang (Vulkan SDK, `C:\VulkanSDK\1.4.363.0\Bin`)

Build output and temp files go to `temp\dxvk_build_x86` and
`temp\msvc_tmp`. Only `d3d9` is built (d3d8/d3d10/d3d11/dxgi disabled), with
a static CRT, so no VC++ redistributable is needed.
