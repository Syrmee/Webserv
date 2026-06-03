import socket
import time

def test_oom_chunked():
    host = "127.0.0.1"
    port = 8080
    
    print(f"[*] Connecting to {host}:{port}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((host, port))
    except ConnectionRefusedError:
        print("[-] Connection refused. Make sure your webserv is running on port 8080!")
        return

    print("[*] Sending headers for chunked POST request...")
    # Using /limit/ to demonstrate that it easily bypasses your 50-byte limit
    headers = (
        "POST /limit/asd HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
    )
    s.sendall(headers.encode('utf-8'))
    
    # Create a 1 Megabyte chunk of letter 'A's
    chunk_size = 1024  
    chunk_data = b"A" * chunk_size
    
    # Format according to RFC: <hex size>\r\n<data>\r\n
    chunk_size_hex = hex(chunk_size)[2:].encode('utf-8')
    chunk_payload = chunk_size_hex + b"\r\n" + chunk_data + b"\r\n"
    
    total_sent_mb = 0
    print("[*] Starting infinite chunk stream.")
    print("[!] Open 'htop' or Activity Monitor to watch your webserv memory usage climb!")
    
    try:
        while True:
            # Send the chunk
            s.sendall(chunk_payload)
            total_sent_mb += 1
            print(f"[+] Sent {total_sent_mb} MB to the server...", end='\r')
            
            # A tiny sleep prevents overwhelming the local OS socket buffers,
            # giving your webserv time to read() and allocate memory for it.
            time.sleep(0.01)
            
    except BrokenPipeError:
        print("\n\n[+] Connection closed by server (Broken Pipe)!")
        print("    If you applied the fix, this means your server successfully protected itself.")
    except ConnectionResetError:
        print("\n\n[+] Connection reset by peer!")
        print("    If you applied the fix, this means your server successfully protected itself.")
    except Exception as e:
        print(f"\n\n[-] Stopped with error: {e}")
    finally:
        s.close()

if __name__ == "__main__":
    test_oom_chunked()
