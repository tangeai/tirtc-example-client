# TiRTC Client Example

Minimal TiRTC client-role downlink example. It connects to a TiRTC device, attaches callback audio/video output sinks, counts frames scheduled by the runtime output boundary, and prints one `TIRTC_CLIENT_SUMMARY` JSON line before exit.

## Prepare Runtime

```sh
./script/prepare.sh
```

By default this downloads the latest `tangeai/tirtc-example-client` GitHub Release asset. For local testing, pass a release zip directly:

```sh
./script/prepare.sh --zip /path/to/YYYYMMDDHHMMSS.zip
```

The prepare step only replaces `3rd/`. It does not modify `src/`, `script/`, `README.md`, or `Makefile`.

## Build

```sh
./script/build.sh
```

Supported prepared runtime platforms are `macos-arm64` and `linux-x64`. Override platform detection with:

```sh
PLATFORM=linux-x64 ./script/build.sh
```

## Run

```sh
./script/run.sh \
  --app-id "$TIRTC_APP_ID" \
  --endpoint "$TIRTC_ENDPOINT" \
  --device-id "$TIRTC_DEVICE_ID" \
  --token "$TIRTC_TOKEN" \
  --audio-stream-id 0 \
  --video-stream-id 1 \
  --duration-seconds 60
```

`--stream-id` is a shorthand for `--video-stream-id`. Stream ids must be in `0..15`; audio and video stream ids must be different when both are enabled.

The sample does not render to a system audio device or window. Audio and video callbacks only update counters and byte totals.

