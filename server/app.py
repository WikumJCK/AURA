"""Minimal server for AURA smart glasses prototype.

This Flask application exposes a /query endpoint that accepts audio and an
optional image from the XIAO ESP32-S3. It returns a synthesized speech response.

The heavy lifting (speech-to-text, vision, text generation and text-to-speech)
are represented here as placeholder functions so the application can be
extended with your preferred AI services.
"""
from flask import Flask, request, send_file
import io

app = Flask(__name__)


def transcribe(audio_bytes: bytes) -> str:
    """Convert raw audio bytes to text.

    TODO: Integrate with Whisper, Google STT or another speech-to-text engine.
    """
    # Placeholder implementation
    return "what am I looking at?"


def describe_image(image_bytes: bytes) -> str:
    """Describe the contents of an image.

    TODO: Use a vision API or local model to extract scene information.
    """
    # Placeholder implementation
    return "a bookshelf and a table"


def synthesize(text: str) -> bytes:
    """Generate spoken audio for the supplied text.

    TODO: Replace with call to a TTS engine and return audio bytes (e.g., WAV).
    """
    # For now, return an empty WAV file header for silence
    return b"RIFF\x24\x08\x00\x00WAVEfmt "


@app.post("/query")
def handle_query():
    audio_file = request.files.get("audio")
    image_file = request.files.get("image")

    if not audio_file:
        return {"error": "audio file required"}, 400

    question = transcribe(audio_file.read())
    context = ""
    if image_file:
        context = describe_image(image_file.read())

    answer = f"You asked: {question}. I can see {context}."
    audio_response = synthesize(answer)
    return send_file(io.BytesIO(audio_response), mimetype="audio/wav")


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000)
