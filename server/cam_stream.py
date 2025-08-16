# Proxy server to view the ESP32 CamServer stream in a browser.
#
# Run with:
#     CAM_HOST=<camera ip> python cam_stream.py
# Then open http://localhost:8080/stream to view the feed.

import os
from flask import Flask, Response, stream_with_context
import requests

BOUNDARY = "123456789000000000000987654321"
CAM_HOST = os.environ.get("CAM_HOST", "192.168.4.1")
CAM_STREAM_URL = f"http://{CAM_HOST}/stream"

app = Flask(__name__)


def _stream_from_camera():
    """Yield bytes from the ESP32 camera stream."""
    with requests.get(CAM_STREAM_URL, stream=True) as r:
        r.raise_for_status()
        for chunk in r.iter_content(chunk_size=1024):
            if chunk:
                yield chunk


@app.route("/stream")
def stream():
    return Response(
        stream_with_context(_stream_from_camera()),
        mimetype=f"multipart/x-mixed-replace; boundary={BOUNDARY}",
    )


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080)
