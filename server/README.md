# AURA Server

This directory contains a minimal Flask server that demonstrates how the smart
glasses can offload heavy AI tasks such as speech recognition, vision and text
generation.

## Usage

```bash
pip install flask
python app.py
```

The `/query` endpoint accepts a `multipart/form-data` POST request containing an
`audio` file and an optional `image` file. The server currently returns a silent
WAV file; integrate your preferred APIs in `app.py` for full functionality.
