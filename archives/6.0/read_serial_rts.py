import serial
import time
import os

# First, let's try to reset the ESP32 by opening the port with specific settings
# that trigger the reset

try:
    # Open port with DTR/RTS to trigger reset
    ser = serial.Serial('COM4', 115200, timeout=1)
    
    # Drop DTR and RTS to reset
    ser.dtr = False
    ser.rts = False
    time.sleep(0.1)
    
    # Flush any existing data
    if ser.in_waiting:
        ser.read(ser.in_waiting)
    
    # Wait for boot loader messages
    print("Waiting for bootloader...")
    time.sleep(3)
    
    if ser.in_waiting:
        data = ser.read(ser.in_waiting)
        print("=== BOOTLOADER ===")
        print(data.decode('utf-8', errors='replace'))
    else:
        print("No bootloader data")
    
    # Now wait for sketch output
    print("\nWaiting for sketch output (5 seconds)...")
    start = time.time()
    while time.time() - start < 5:
        if ser.in_waiting:
            data = ser.read(ser.in_waiting)
            print("=== SKETCH ===")
            print(data.decode('utf-8', errors='replace'))
        time.sleep(0.1)
    
    ser.close()
except Exception as e:
    print(f"Error: {e}")
