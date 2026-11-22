# DaliCam

Sprint 0 foundation for a CardputerZero app that will eventually handle Gemini image generation, camera capture, and a simple web UI.

## Current status

This repository currently proves the **build / package / deploy / launch** path with a minimal hello screen, and Sprint 1 now adds the first runtime plumbing.

- App name: `DaliCam`
- Package name: `dalicam`
- Current UI: main screen prompt entry with image preview rendered through an LVGL canvas, plus a settings screen for Gemini API key, personality selector, and image model selector
- Web UI: home page on port 8080 with inline latest image, image upload, API key / model / personality settings, and viewer endpoints for the latest camera, generated, and uploaded images
- Config file: `config/dalicam.conf`
- Runtime app data: stored under the user profile on the SD-backed filesystem in `~/.local/share/DaliCam` by default
- Runtime settings file: `~/.local/share/DaliCam/settings.ini`
- Runtime folders: `input/` (camera captures), `generated/` (Gemini outputs), `uploads/` (manual uploads), `temp/`, `logs/`
- Allowed image models: `gemini-nano-banana-2.1`, `gemini-3.1-flash-lite-image`, `gemini-3-pro-image`
- Personality presets: Dali first, with room for Vector and custom presets later

## Why Sprint 0 exists

The first goal is to confirm that we are building for the correct CardputerZero app path before adding any Gemini, camera, or web UI code.

If Sprint 0 works, we keep this project as the base and build directly on top of it.

## Build

Typical workflow:

```bash
cmake --workflow --preset cp0-cross-package
```

The project also includes a `deploy.sh` helper that expects the package in `dist/`.

## Deploy

Example:

```bash
REMOTE_USER=pi REMOTE_HOST=192.168.1.10 ./deploy.sh
```

This is meant to install only the app package over SSH, not replace the CardputerZero OS.

## Next planned work

1. Polish the main screen prompt flow and preview layout now that the canvas preview path is working
2. Add on-device galleries for captured, uploaded, and AI-generated images
3. Add a WebUI gallery with day grouping and image inspection tools
4. Wire image selection into the prompt-to-image flow so an uploaded or captured image can be edited with the preset prompt
5. Condense the main screen header/status area to make room for gallery navigation and metadata
6. Refine the saved settings and prompt editing flow

## Notes

- The current app is intentionally simple.
- The prompt box now lives on the main screen.
- Pressing Enter on the main screen sends the prompt directly to the Gemini image generation flow; it does not need a chat-style command.
- The main screen now includes separate **Settings** and **Capture** buttons.
- Camera photos are saved under `input/camera/` and Gemini outputs are saved under `generated/`.
- Uploaded test images are stored under `uploads/` so we can compare them against camera and generated images.
- The device preview now uses a canvas/raw-buffer render path, which fixed the mosaic rendering issue on the screen.
- The WebUI home page now shows the latest image inline and supports image upload for testing the preview pipeline.
- After capture, the app shows a small confirm prompt:
  - **Y** uses the captured photo with the current prompt/default Dali prompt
  - **N** deletes the captured photo and returns to the prompt screen
- We are using this repo as the base shell for the final app.
- The release package is designed to install over SSH with `sudo -n dpkg -i` without replacing the CardputerZero OS.
- If the prompt is left blank, the Dali default prompt is used automatically.
