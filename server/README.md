# Spoken Command Server

Local HTTP bridge between the ESP32-C6 board and ElevenLabs Speech to Text.

## Run

```sh
cp .env.example .env.local
$EDITOR .env.local
source .env.local
python3 server.py
```

The server listens on `0.0.0.0:8080` by default.

## Endpoints

- `GET /health` returns a basic health check.
- `GET /commands/recent` returns recent transcripts kept in memory.
- `POST /audio/command` accepts either WAV or raw PCM audio. Raw PCM may use a
  normal `Content-Length` request or HTTP chunked transfer.

For raw PCM, send:

```text
Content-Type: application/octet-stream
X-Audio-Sample-Rate: 16000
X-Audio-Channels: 1
X-Device-Id: waveshare-c6
```

The current ElevenLabs STT call uses `POST https://api.elevenlabs.io/v1/speech-to-text`,
`model_id=scribe_v2`, and a multipart `file` field.
