import cv2
cap = cv2.VideoCapture("http://192.168.8.80:81/stream")
while True:
    ok, frame = cap.read()
    if not ok: continue
    frame = cv2.rotate(frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
    cv2.imshow("AURA Stream (cv2 cap)", frame)
    if cv2.waitKey(1) & 0xFF == ord('q'): break
cap.release()
cv2.destroyAllWindows()
