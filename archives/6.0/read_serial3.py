import serial
import time

try:
    ser = serial.Serial('COM4', 115200, timeout=1)
    time.sleep(2)
    
    # Read initial data
    if ser.in_waiting:
        data = ser.read(ser.in_waiting)
        print("=== INITIAL DATA ===")
        print(data.decode('utf-8', errors='replace'))
    
    # Wait a bit more and check again
    time.sleep(3)
    if ser.in_waiting:
        data = ser.read(ser.in_waiting)
        print("=== MORE DATA ===")
        print(data.decode('utf-8', errors='replace'))
    
    ser.close()
except Exception as e:
    print(f"Error: {e}")
