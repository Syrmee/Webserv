import socket
import select
import time

def test_oom_chunked_with_status():
    host = "127.0.0.1"
    port = 8080
    
    print(f"[*] Connecting to {host}:{port}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((host, port))
        # Set socket to non-blocking so we can read responses on the fly
        s.setblocking(0)
    except ConnectionRefusedError:
        print("[-] Connection refused. Make sure your webserv is running on port 8080!")
        return

    print("[*] Sending headers for chunked POST request...")
    headers = (
        "POST /limit/asd HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
    )
    
    # Wait until socket is writable to send headers
    _, writable, _ = select.select([], [s], [], 5.0)
    if writable:
        s.sendall(headers.encode('utf-8'))
    
    # Using 8 KB chunks to bypass your accidental 32KB header limit trap
    chunk_size = 10#8192
    chunk_data = b"A" * chunk_size
    chunk_size_hex = hex(chunk_size)[2:].encode('utf-8')
    chunk_payload = chunk_size_hex + b"\r\n" + chunk_data + b"\r\n"
    
    total_sent_mb = 0
    chunks_sent = 0
    print("[*] Starting infinite chunk stream.")
    print("[*] Listening for server response concurrently...\n")
    
    try:
        while True:
            # Check if the server sent data (readable) OR if we can send data (writable)
            readable, writable, exceptional = select.select([s], [s], [s], 0.05)
            
            # 1. Did the server send us an HTTP response?
            if readable:
                try:
                    response = s.recv(4096)
                    if response:
                        print("\n" + "="*50)
                        print("[+] SERVER REPLIED BEFORE CLOSING CONNECTION:")
                        response_str = response.decode('utf-8', errors='ignore')
                        
                        # Extract and print the HTTP Status Line
                        status_line = response_str.split('\r\n')[0]
                        print(f"    --> {status_line} <--")
                        
                        # Print the rest of the headers
                        headers_only = response_str.split('\r\n\r\n')[0]
                        print("\n    Full Headers:")
                        for line in headers_only.split('\r\n')[1:]:
                            print(f"      {line}")
                        print("="*50 + "\n")
                        break
                    else:
                        print("\n[-] Server closed connection without sending an HTTP response.")
                        break
                except BlockingIOError:
                    pass

            # 2. If the server hasn't replied yet, send another chunk
            if writable:
                s.sendall(chunk_payload)
                chunks_sent += 1
                
                # Print progress every 1 MB
                if chunks_sent * chunk_size >= 1024 * 1024:
                    total_sent_mb += 1
                    chunks_sent = 0
                    print(f"[+] Sent {total_sent_mb} MB to the server...", end='\r')
                    
    except BrokenPipeError:
        print("\n\n[-] Connection closed by server (Broken Pipe) before we could read the status!")
    except ConnectionResetError:
        print("\n\n[-] Connection reset by peer before we could read the status!")
    except Exception as e:
        print(f"\n\n[-] Stopped with error: {e}")
    finally:
        s.close()

if __name__ == "__main__":
    test_oom_chunked_with_status()
