import serial
import time
import threading

received_data = []
stop_thread = False

def read_serial():
    global received_data, stop_thread
    try:
        ser = serial.Serial('COM4', 115200, timeout=0.1)
        while not stop_thread:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting)
                text = data.decode('utf-8', errors='replace')
                received_data.append(text)
                print(text, end='', flush=True)
            time.sleep(0.1)
        ser.close()
    except Exception as e:
        print(f"Error: {e}")

# Start reading in background
reader = threading.Thread(target=read_serial, daemon=True)
reader.start()

# Wait for data
print("Waiting for serial data...")
time.sleep(8)

# Stop reading
stop_thread = True
time.sleep(1)

print("\n=== SUMMARY ===")
print(f"Received {len(received_data)} chunks")
print("".join(received_data))
