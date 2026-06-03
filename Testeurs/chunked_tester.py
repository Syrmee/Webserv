#!/usr/bin/env python3

import time

import socket
import time

def send_manual_chunked_request(host, port, path, total_chunks=10):
    # Establish a standard TCP connection
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_socket.settimeout(5.0)  # Standard timeout to prevent hanging

    try:
        client_socket.connect((host, port))
        print(f"Connected to {host}:{port}")

        # 1. Construct and send the HTTP headers
        # The header must specify 'Transfer-Encoding: chunked'
        http_headers = (
            f"POST {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Transfer-Encoding: chunked\r\n"
            f"Content-Type: text/plain\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        )
        client_socket.sendall(http_headers.encode('utf-8'))

        # 2. Stream a bounded number of chunks
        for i in range(total_chunks):
            payload = f"Log entry line reference: #{i}\n".encode('utf-8')
            
            # Format: <HEX_SIZE>\r\n<DATA>\r\n
            chunk_header = f"{len(payload):X}\r\n".encode('utf-8')
            chunk_data = payload + b"\r\n"
            
            # Send the size and the payload
            client_socket.sendall(chunk_header + chunk_data)
            print(f"Sent chunk {i+1}/{total_chunks}")
            
            time.sleep(0.2)  # Simulating processing time between data availability

        # 3. Send the mandatory terminating chunk (0 length)
        # This signals to the server that the request body is complete
        final_chunk = b"0\r\n\r\n"
        client_socket.sendall(final_chunk)
        print("Sent termination chunk. Waiting for response...")

        # 4. Read the server's response
        response = b""
        while True:
            data = client_socket.recv(4096)
            if not data:
                break
            response += data
        
        print("\n--- Server Response ---")
        print(response.decode('utf-8', errors='ignore'))

    except socket.timeout:
        print("The connection timed out.")
    except Exception as e:
        print(f"Network error: {e}")
    finally:
        client_socket.close()
        print("Socket closed safely.")	

def send_entire_chunked_request(host, port, path):
    # Establish a standard TCP connection
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_socket.settimeout(5.0)

    try:
        client_socket.connect((host, port))
        print(f"Connected to {host}:{port}")

        # 1. Define the raw string payloads for the chunks
        payload_1 = b"First chunk of text data\n"
        payload_2 = b"Second chunk containing more data\n"
        payload_3 = b"Final piece of data in the sequence\n"

        # 2. Construct the HTTP Header block
        # Must include 'Transfer-Encoding: chunked' and end with \r\n\r\n
        headers = (
            f"POST {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Transfer-Encoding: chunked\r\n"
            f"Content-Type: text/plain\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode('utf-8')

        # 3. Format each chunk into the HTTP/1.1 specification:
        # <HEX_SIZE>\r\n<DATA>\r\n
        chunk_1 = f"{len(payload_1):X}\r\n".encode('utf-8') + payload_1 + b"\r\n"
        chunk_2 = f"{len(payload_2):X}\r\n".encode('utf-8') + payload_2 + b"\r\n"
        chunk_3 = f"{len(payload_3):X}\r\n".encode('utf-8') + payload_3 + b"\r\n"

        # 4. The protocol requires a final empty chunk to denote completion
        termination_chunk = b"0\r\n\r\n"

        # 5. Concatenate everything into a single, comprehensive byte buffer
        full_packet = headers + chunk_1 + chunk_2 + chunk_3 + termination_chunk

        print(f"Assembling packet... Total size to transmit: {len(full_packet)} bytes.")

        # 6. Transmit the entire packet in a single atomic network operation
        client_socket.sendall(full_packet)
        print("Entire chunked request transmitted in one sendall call.")

        # 7. Read the resulting server response
        response = b""
        while True:
            data = client_socket.recv(4096)
            if not data:
                break
            response += data
        
        print("\n--- Server Response ---")
        print(response.decode('utf-8', errors='ignore'))

    except socket.timeout:
        print("The connection timed out waiting for a server response.")
    except Exception as e:
        print(f"Network error encountered: {e}")
    finally:
        client_socket.close()
        print("Socket closed safely.")


def send_request_multiple_send(host, port, path):
    # Establish a standard TCP connection
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_socket.settimeout(5.0)  # Standard timeout to prevent hanging

    try:
        client_socket.connect((host, port))
        print(f"Connected to {host}:{port}")

        # 1. Construct and send the HTTP headers
        # The header must specify 'Transfer-Encoding: chunked'
        http_line = (
            f"GET {path} HTTP/1.1\r\n"
        )
        client_socket.sendall(http_line.encode('utf-8'))

        time.sleep(0.2)  # Simulating processing time between data availability

        http_fields = (
            f"Host: {host}\r\n"
            f"Transfer-Encoding: chunked\r\n"
            f"Content-Type:"
        )
        client_socket.sendall(http_fields.encode('utf-8'))
        time.sleep(0.2)  # Simulating processing time between data availability

        http_fields = (
            f" text/plain\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        )
        client_socket.sendall(http_fields.encode('utf-8'))
        time.sleep(0.2)  # Simulating processing time between data availability

        print("Sent terminated. Waiting for response...")


        # 4. Read the server's response
        response = b""
        while True:
            data = client_socket.recv(4096)
            if not data:
                break
            response += data
        
        print("\n--- Server Response ---")
        print(response.decode('utf-8', errors='ignore'))

    except socket.timeout:
        print("The connection timed out.")
    except Exception as e:
        print(f"Network error: {e}")
    finally:
        client_socket.close()
        print("Socket closed safely.")	

if __name__ == "__main__":
    send_manual_chunked_request('localhost', 8080, '/post_body')
    send_entire_chunked_request('localhost', 8080, '/post_body')