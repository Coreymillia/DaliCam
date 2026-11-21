# DaliCam

Sprint 0 foundation for a CardputerZero app that will eventually handle Gemini image generation, camera capture, and a simple web UI.

## Current status

This repository currently proves the **build / package / deploy / launch** path with a minimal hello screen.

- App name: `DaliCam`
- Package name: `dalicam`
- Current UI: `HELLO`
- Config file: `config/dalicam.conf`

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

1. Verify the hello app launches on the device
2. Add shared paths/settings/storage modules
3. Port Gemini image generation from WhisplayGroqHat
4. Add camera capture and confirm Y/N flow
5. Add the simplified web UI

## Notes

- The current app is intentionally simple.
- The Dali and Vector prompt work will come later.
- We are using this repo as the base shell for the final app.
- The release package is designed to install over SSH with `sudo -n dpkg -i` without replacing the CardputerZero OS.
