#!/usr/bin/env python3
"""Local bridge server for ESP32 spoken-command audio.

The ESP32 posts either WAV bytes or raw signed 16-bit little-endian PCM. The
server wraps PCM as WAV, forwards it to ElevenLabs Speech to Text, interprets
the transcript as a local command, and returns a compact device response.
"""

from __future__ import annotations

import io
import json
import os
import time
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


HOST = os.environ.get("COMMAND_SERVER_HOST", "0.0.0.0")
PORT = int(os.environ.get("COMMAND_SERVER_PORT", "8080"))
ELEVENLABS_URL = "https://api.elevenlabs.io/v1/speech-to-text"
MODEL_ID = os.environ.get("ELEVENLABS_MODEL_ID", "scribe_v2")
MAX_AUDIO_BYTES = int(os.environ.get("COMMAND_SERVER_MAX_AUDIO_BYTES", str(4 * 1024 * 1024)))

RECENT_COMMANDS: list[dict[str, Any]] = []


def json_response(handler: BaseHTTPRequestHandler, status: int, payload: dict[str, Any]) -> None:
    body = json.dumps(payload, indent=2).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def read_request_body(handler: BaseHTTPRequestHandler) -> bytes:
    if handler.headers.get("Transfer-Encoding", "").lower() == "chunked":
        chunks: list[bytes] = []
        total = 0

        while True:
            size_line = handler.rfile.readline(64)
            if not size_line:
                raise ValueError("incomplete chunked request")
            size_text = size_line.split(b";", 1)[0].strip()
            chunk_size = int(size_text, 16)
            if chunk_size == 0:
                handler.rfile.readline(2)
                break
            total += chunk_size
            if total > MAX_AUDIO_BYTES:
                raise ValueError(f"audio body too large: {total} bytes")
            chunks.append(handler.rfile.read(chunk_size))
            if handler.rfile.read(2) != b"\r\n":
                raise ValueError("invalid chunk terminator")

        return b"".join(chunks)

    content_length = int(handler.headers.get("Content-Length", "0"))
    if content_length <= 0:
        raise ValueError("missing request body")
    if content_length > MAX_AUDIO_BYTES:
        raise ValueError(f"audio body too large: {content_length} bytes")
    return handler.rfile.read(content_length)


def pcm_s16le_to_wav(pcm: bytes, sample_rate: int, channels: int) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return output.getvalue()


def multipart_form(fields: dict[str, str], file_field: str, filename: str, content_type: str, data: bytes) -> tuple[bytes, str]:
    boundary = f"----spoken-command-{uuid.uuid4().hex}"
    chunks: list[bytes] = []

    for name, value in fields.items():
        chunks.append(f"--{boundary}\r\n".encode("ascii"))
        chunks.append(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode("ascii"))
        chunks.append(value.encode("utf-8"))
        chunks.append(b"\r\n")

    chunks.append(f"--{boundary}\r\n".encode("ascii"))
    chunks.append(
        (
            f'Content-Disposition: form-data; name="{file_field}"; filename="{filename}"\r\n'
            f"Content-Type: {content_type}\r\n\r\n"
        ).encode("ascii")
    )
    chunks.append(data)
    chunks.append(b"\r\n")
    chunks.append(f"--{boundary}--\r\n".encode("ascii"))

    return b"".join(chunks), boundary


def transcribe_with_elevenlabs(wav_bytes: bytes) -> dict[str, Any]:
    api_key = os.environ.get("ELEVENLABS_API_KEY")
    if not api_key:
        raise RuntimeError("ELEVENLABS_API_KEY is not set")

    body, boundary = multipart_form(
        fields={"model_id": MODEL_ID},
        file_field="file",
        filename="command.wav",
        content_type="audio/wav",
        data=wav_bytes,
    )
    request = Request(
        ELEVENLABS_URL,
        data=body,
        headers={
            "xi-api-key": api_key,
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Accept": "application/json",
        },
        method="POST",
    )

    try:
        with urlopen(request, timeout=60) as response:
            return json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"ElevenLabs returned HTTP {exc.code}: {detail}") from exc
    except URLError as exc:
        raise RuntimeError(f"ElevenLabs request failed: {exc.reason}") from exc


def normalize_command_text(text: str) -> str:
    return " ".join(text.strip().lower().split())


def command_response(transcript_text: str) -> dict[str, Any]:
    text = transcript_text.strip()
    normalized = normalize_command_text(text)

    if not text:
        return {
            "ok": False,
            "transcript": "",
            "display_text": "No speech heard.",
            "tone": "error",
        }

    if normalized in {"test", "ping"}:
        return {
            "ok": True,
            "transcript": text,
            "display_text": "Ready.",
            "tone": "success",
        }

    if normalized in {"help", "commands", "what can you do"}:
        return {
            "ok": True,
            "transcript": text,
            "display_text": "Commands: test, status, repeat.",
            "tone": "success",
        }

    if normalized in {"status", "server status"}:
        return {
            "ok": True,
            "transcript": text,
            "display_text": "Server online.",
            "tone": "success",
        }

    for prefix in ("repeat ", "say "):
        if normalized.startswith(prefix):
            display_text = text[len(prefix):].strip()
            return {
                "ok": bool(display_text),
                "transcript": text,
                "display_text": display_text or "Nothing to repeat.",
                "tone": "success" if display_text else "error",
            }

    return {
        "ok": True,
        "transcript": text,
        "display_text": f"Heard: {text}",
        "tone": "success",
    }


class CommandHandler(BaseHTTPRequestHandler):
    server_version = "SpokenCommandServer/0.1"

    def do_GET(self) -> None:
        if self.path == "/health":
            json_response(self, 200, {"ok": True, "service": "spoken-command-server"})
            return
        if self.path == "/commands/recent":
            json_response(self, 200, {"commands": RECENT_COMMANDS[-20:]})
            return
        json_response(self, 404, {"error": "not found"})

    def do_POST(self) -> None:
        if self.path != "/audio/command":
            json_response(self, 404, {"error": "not found"})
            return

        try:
            body = read_request_body(self)
            content_type = self.headers.get("Content-Type", "application/octet-stream").split(";")[0].strip()
            sample_rate = int(self.headers.get("X-Audio-Sample-Rate", "16000"))
            channels = int(self.headers.get("X-Audio-Channels", "1"))
            device_id = self.headers.get("X-Device-Id", "unknown")

            if content_type in ("audio/wav", "audio/x-wav"):
                wav_bytes = body
            elif content_type in ("application/octet-stream", "audio/pcm"):
                wav_bytes = pcm_s16le_to_wav(body, sample_rate, channels)
            else:
                raise ValueError(f"unsupported Content-Type: {content_type}")

            started = time.monotonic()
            transcript = transcribe_with_elevenlabs(wav_bytes)
            device_response = command_response(transcript.get("text", ""))
            record = {
                "device_id": device_id,
                "received_at": int(time.time()),
                "duration_ms": int((time.monotonic() - started) * 1000),
                "text": device_response["transcript"],
                "display_text": device_response["display_text"],
                "tone": device_response["tone"],
                "transcript": transcript,
            }
            RECENT_COMMANDS.append(record)
            del RECENT_COMMANDS[:-50]
            json_response(self, 200, device_response)
        except Exception as exc:
            json_response(self, 400, {
                "ok": False,
                "transcript": "",
                "display_text": "Command failed.",
                "tone": "error",
                "error": str(exc),
            })

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"{self.address_string()} - {fmt % args}")


def main() -> None:
    server = ThreadingHTTPServer((HOST, PORT), CommandHandler)
    print(f"Listening on http://{HOST}:{PORT}")
    print("POST audio to /audio/command with ELEVENLABS_API_KEY set in the environment.")
    server.serve_forever()


if __name__ == "__main__":
    main()
