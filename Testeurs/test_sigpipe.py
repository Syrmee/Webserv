#!/usr/bin/env python3
import socket
import time

def trigger_sigpipe():
    print("1. Connecting to server...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    
    print("2. Sending request...")
    s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    
    # Read just 1 byte to ensure the server starts sending
    s.recv(1)
    
    print("3. Maliciously closing the connection instantly (TCP RST)...")
    # Linger 0 forces an abrupt TCP RST instead of a graceful FIN
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\x01\x00\x00\x00\x00\x00\x00\x00')
    s.close()
    
    print("Done. Look at your server terminal. Did it crash?")

if __name__ == "__main__":
    trigger_sigpipe()
