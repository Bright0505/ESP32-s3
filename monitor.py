import serial, time

ser = serial.Serial('/dev/cu.usbmodem1101', 115200, timeout=0.2)
ser.dtr = False
ser.rts = False

end = time.time() + 600
with open('log.txt', 'w') as log:
    while time.time() < end:
        data = ser.read(512)
        if data:
            text = data.decode('utf-8', errors='replace')
            print(text, end='', flush=True)
            log.write(text)
            log.flush()

ser.close()
print('\nLog saved to log.txt')
