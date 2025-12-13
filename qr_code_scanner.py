import cv2
import numpy as np
from pyzbar.pyzbar import decode
import time

# Function to capture image from ESP32-CAM
def capture_frame(url):
    cap = cv2.VideoCapture(url)
    ret, frame = cap.read()
    cap.release()
    if not ret:
        raise Exception("Failed to capture frame from ESP32-CAM")
    return frame

# Function to scan QR codes in the frame
def scan_qr_code(frame):
    decoded_objects = decode(frame)
    qr_data = []
    for obj in decoded_objects:
        qr_data.append(obj.data.decode('utf-8'))
    return qr_data

# Log laps using QR codes
def log_laps(qr_log_file, url):
    with open(qr_log_file, 'a') as f:
        while True:
            try:
                frame = capture_frame(url)
                qr_codes = scan_qr_code(frame)
                if qr_codes:
                    current_time = time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime())
                    for qr_code in qr_codes:
                        f.write(f"{current_time}, {qr_code}\n")
                        print(f"Lap logged: {qr_code} at {current_time}")
                time.sleep(1)  # Add delay to avoid processing the same QR repeatedly
            except KeyboardInterrupt:
                print("Logging stopped.")
                break
            except Exception as e:
                print(f"Error: {e}")

# Example usage
# log_laps('lap_log.txt', 'http://<ESP32-CAM-URL>/stream')