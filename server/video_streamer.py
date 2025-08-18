# aura_periodic_capture_caption_local.py
# Requires: opencv-python, numpy, pillow, transformers, torch (you said torch is installed)
# pip install opencv-python numpy pillow transformers

import os, time, cv2, numpy as np, datetime as dt
from PIL import Image
import torch
from transformers import BlipProcessor, BlipForConditionalGeneration

# ==== CONFIG ====
ESP32_IP      = "192.168.8.80"        # your device IP
STREAM_URL    = f"http://{ESP32_IP}:81/stream"
ROTATE_MODE   = "ccw90"               # "none" | "cw90" | "ccw90" | "rot180"
CAPTURE_EVERY = 10.0                  # seconds between captures
SAVE_DIR      = "captures"
SHOW_WINDOW   = True                  # False = headless
MODEL_NAME    = "Salesforce/blip-image-captioning-base"  # smaller & faster than -large
MAX_TOKENS    = 30                    # caption length budget
# =================

os.makedirs(SAVE_DIR, exist_ok=True)

# Pick device (CPU by default; use "cuda" if you want & have it)
DEVICE = "cpu"  # change to "cuda" if you have a GPU ready

print("[INIT] Loading BLIP model locally… (first run downloads weights)")
processor = BlipProcessor.from_pretrained(MODEL_NAME)
model = BlipForConditionalGeneration.from_pretrained(MODEL_NAME)
model.to(DEVICE)
model.eval()

def rotate_frame(img, mode):
    if mode == "cw90":   return cv2.rotate(img, cv2.ROTATE_90_CLOCKWISE)
    if mode == "ccw90":  return cv2.rotate(img, cv2.ROTATE_90_COUNTERCLOCKWISE)
    if mode == "rot180": return cv2.rotate(img, cv2.ROTATE_180)
    return img

@torch.no_grad()
def caption_image_local_bgr(frame_bgr):
    """Takes a BGR OpenCV frame, returns caption string using BLIP (local)."""
    # Convert BGR -> RGB PIL Image
    rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
    pil = Image.fromarray(rgb)

    inputs = processor(images=pil, return_tensors="pt").to(DEVICE)
    out_ids = model.generate(**inputs, max_new_tokens=MAX_TOKENS)
    text = processor.decode(out_ids[0], skip_special_tokens=True)
    return text

def main():
    cap = cv2.VideoCapture(STREAM_URL)
    if not cap.isOpened():
        print(f"[ERR] Cannot open stream: {STREAM_URL}")
        return

    print("Streaming… Press 'q' to quit.")
    last_save = 0.0

    while True:
        ok, frame = cap.read()
        if not ok:
            time.sleep(0.01)
            continue

        frame = rotate_frame(frame, ROTATE_MODE)

        now = time.time()
        if now - last_save >= CAPTURE_EVERY:
            last_save = now
            ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
            img_path = os.path.join(SAVE_DIR, f"{ts}.jpg")
            cv2.imwrite(img_path, frame)
            print(f"[SAVE] {img_path}")

            # Local caption
            caption = caption_image_local_bgr(frame)
            print(f"[CAPTION] {caption}")

            # Optional: overlay the caption for a moment
            if SHOW_WINDOW:
                overlay = frame.copy()
                txt = caption[:180]
                # Draw a background box for readability
                cv2.rectangle(overlay, (8, 8), (8 + 1000, 8 + 60), (0, 0, 0), -1)
                cv2.putText(overlay, txt, (16, 48),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.9, (255, 255, 255), 2,
                            cv2.LINE_AA)
                cv2.imshow("AURA Stream (local captions)", overlay)
                cv2.waitKey(1)

        if SHOW_WINDOW:
            cv2.imshow("AURA Stream (local captions)", frame)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

    cap.release()
    if SHOW_WINDOW:
        cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
