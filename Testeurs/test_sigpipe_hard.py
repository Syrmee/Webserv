#!/usr/bin/env python3
import socket
import time

def trigger_hard_sigpipe():
    print("1. Connecting to server...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    
    print("2. Requesting the 10MB large.bin file...")
    s.sendall(b"GET /large.bin HTTP/1.1\r\nHost: localhost\r\n\r\n")
    
    print("3. Reading just a tiny chunk so the server starts its send loop...")
    s.recv(1024)
    
    print("4. Maliciously closing the connection instantly (TCP RST)...")
    # Linger 0 forces an abrupt TCP RST instead of a graceful FIN
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\x01\x00\x00\x00\x00\x00\x00\x00')
    s.close()
    
    print("Done. Look at your server terminal. It should have crashed with 'Broken pipe'.")

if __name__ == "__main__":
    trigger_hard_sigpipe()