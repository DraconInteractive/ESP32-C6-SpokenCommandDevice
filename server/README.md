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
  normal `Content-Length` request or HTTP chunked transfer. It returns a compact
  device response:

```json
{
  "ok": true,
  "transcript": "test",
  "command": "test",
  "display_text": "Ready.",
  "tone": "success",
  "state": {
    "muted": false
  }
}
```

For raw PCM, send:

```text
Content-Type: application/octet-stream
X-Audio-Sample-Rate: 16000
X-Audio-Channels: 1
X-Device-Id: waveshare-c6
```

The current ElevenLabs STT call uses `POST https://api.elevenlabs.io/v1/speech-to-text`,
`model_id=scribe_v2`, and a multipart `file` field.

## Built-in Commands

- `test` or `ping` returns `Ready.`
- `status` returns `Server online.`
- `help` returns a short command list.
- `cancel` cancels a pending command prompt.
- `mute` disables response tones for that device.
- `unmute` enables response tones for that device.
- `repeat ...` or `say ...` displays the spoken suffix.
- `timer`, `set timer`, or `set a timer` asks for a duration if one was not
  supplied. The next response from the same device completes the timer.
- Anything else displays `Heard: ...`.

Example timer exchange:

```text
User: set a timer
Device: How long should the timer be?
User: 5 minutes
Device: Timer set: 5 min
```
