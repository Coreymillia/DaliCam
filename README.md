# DaliCam

Sprint 0 foundation for a CardputerZero app that will eventually handle Gemini image generation, camera capture, and a simple web UI.

## Current status

This repository currently proves the **build / package / deploy / launch** path with a minimal hello screen, and Sprint 1 now adds the first runtime plumbing.

- App name: `DaliCam`
- Package name: `dalicam`
- Current UI: main screen prompt entry with image preview, plus a settings screen for Gemini API key, personality selector, and image model selector
- Config file: `config/dalicam.conf`
- Runtime app data: stored under the user profile on the SD-backed filesystem in `~/.local/share/DaliCam` by default
- Runtime settings file: `~/.local/share/DaliCam/settings.ini`
- Runtime folders: `input/`, `generated/`, `temp/`, `logs/`
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

1. Polish the main screen prompt flow and preview behavior
2. Keep settings focused on API key, model, and personality only
3. Wire the camera capture flow
4. Add confirm Y/N handling after capture
5. Add the simplified web UI
6. Refine the saved settings and prompt editing flow

## Notes

- The current app is intentionally simple.
- The prompt box now lives on the main screen.
- Pressing Enter on the main screen sends the prompt directly to the Gemini image generation flow; it does not need a chat-style command.
- We are using this repo as the base shell for the final app.
- The release package is designed to install over SSH with `sudo -n dpkg -i` without replacing the CardputerZero OS.
- If the prompt is left blank, the Dali default prompt is used automatically.
