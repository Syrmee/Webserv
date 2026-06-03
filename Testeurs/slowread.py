import socket
import time

def test_blocking():
    print("[1] Connecting to server...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", 8080))
    
    print("[2] Requesting huge file...")
    request = "GET /huge_file.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
    s.send(request.encode())
    
    print("[3] Reading only 10 bytes, then going to sleep for 60 seconds...")
    data = s.recv(10)
    print(f"    Received: {data}")
    
    print("[4] Try running 'curl http://127.0.0.1:8080/' in another terminal NOW!")
    time.sleep(60) # Refuse to read any more data, causing server's send() to block
    
    s.close()

if __name__ == "__main__":
    test_blocking()
