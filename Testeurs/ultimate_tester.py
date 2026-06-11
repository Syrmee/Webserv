#!/usr/bin/env python3
import os
import socket
import http.client
import time

GREEN = '\033[92m'
RED = '\033[91m'
RESET = '\033[0m'

def setup_environment():
    print("[*] Setting up dummy filesystem for tests...")
    base = "/tmp/www/ultimate"
    dirs = [
        f"{base}/www",
        f"{base}/www/upload",
        f"{base}/www8081"
    ]
    for d in dirs:
        os.makedirs(d, exist_ok=True)

    with open(f"{base}/www/index.html", "w") as f:
        f.write("<html><body><h1>Main Server 8080</h1></body></html>")
        
    with open(f"{base}/www8081/index.html", "w") as f:
        f.write("<html><body><h1>Backup Server 8081</h1></body></html>")
        
    with open(f"{base}/www/custom_404.html", "w") as f:
        f.write("<html><body><h1>Custom 404 Page</h1></body></html>")

    cgi_script = f"{base}/www/test.py"
    with open(cgi_script, "w") as f:
        f.write("#!/usr/bin/env python3\n")
        f.write("import sys\n")
        f.write("print('Status: 200 OK\\r')\n")
        f.write("print('Content-Type: text/plain\\r')\n")
        f.write("print('\\r')\n")
        f.write("print('CGI WORKS')\n")
        f.write("body = sys.stdin.read()\n")
        f.write("if body:\n")
        f.write("    print('BODY:' + body)\n")
    os.chmod(cgi_script, 0o755)
    print("[+] Filesystem ready.\n")

def assert_test(name, condition, error_msg):
    if condition:
        print(f"{GREEN}[PASS]{RESET} {name}")
    else:
        print(f"{RED}[FAIL]{RESET} {name} - {error_msg}")

def run_tests():
    # 1. Test GET on Port 8080
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("GET", "/")
    res = conn.getresponse()
    assert_test("GET / (Port 8080)", res.status == 200 and b"Main Server" in res.read(), "Failed to get root index")
    conn.close()

    # 2. Test GET on Port 8081 (Multiple ports routing)
    conn = http.client.HTTPConnection("127.0.0.1", 8081)
    conn.request("GET", "/")
    res = conn.getresponse()
    assert_test("GET / (Port 8081)", res.status == 200 and b"Backup Server" in res.read(), "Failed multi-port routing")
    conn.close()

    # 3. Test Custom Error Page (404)
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("GET", "/nonexistent")
    res = conn.getresponse()
    assert_test("Custom 404 Page", res.status == 404 and b"Custom 404 Page" in res.read(), "Did not serve custom error page")
    conn.close()

    # 4. Test Method Not Allowed (405)
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("POST", "/")
    res = conn.getresponse()
    assert_test("Method Not Allowed (405)", res.status == 405, f"Expected 405, got {res.status}")
    conn.close()

    # 5. Test Body Limit (413)
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("POST", "/limit", body="A" * 100) # Limit is 50
    res = conn.getresponse()
    assert_test("Body Size Limit (413)", res.status == 413, f"Expected 413, got {res.status}")
    conn.close()

    # 6. Test Unknown Method (501 or 400) using raw socket to bypass http.client protections
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", 8080))
    s.sendall(b"FUBAR / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
    response = s.recv(4096)
    s.close()
    assert_test("Unknown Request Method", b"501 Not Implemented" in response or b"400 Bad Request" in response, "Crash or wrong code for FUBAR")

    # 7. Test File Upload (POST -> 201)
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("POST", "/upload/eval_test.txt", body="EVALUATION UPLOAD TEST")
    res = conn.getresponse()
    assert_test("File Upload (201 Created)", res.status == 201, f"Expected 201, got {res.status}")
    conn.close()

    # 8. Test Retrieve Uploaded File (GET -> 200)
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("GET", "/upload/eval_test.txt")
    res = conn.getresponse()
    assert_test("Retrieve Uploaded File", res.status == 200 and b"EVALUATION UPLOAD TEST" in res.read(), "File missing or corrupted")
    conn.close()

    # 9. Test Autoindex
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("GET", "/upload/")
    res = conn.getresponse()
    assert_test("Autoindex Directory Listing", res.status == 200 and b"eval_test.txt" in res.read(), "Autoindex failed")
    conn.close()

    # 10. Test DELETE Method
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("DELETE", "/delete/eval_test.txt")
    res = conn.getresponse()
    assert_test("DELETE Request (204 No Content)", res.status == 204, f"Expected 204, got {res.status}")
    conn.close()

    # 11. Verify Deletion (404)
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("GET", "/upload/eval_test.txt")
    res = conn.getresponse()
    assert_test("Verify File Deletion (404)", res.status == 404, "File still exists after DELETE")
    conn.close()

    # 12. Test CGI GET
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("GET", "/cgi-bin/test.py")
    res = conn.getresponse()
    assert_test("CGI GET Execution", res.status == 200 and b"CGI WORKS" in res.read(), "CGI GET failed")
    conn.close()

    # 13. Test CGI POST
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("POST", "/cgi-bin/test.py", body="HELLO_FROM_TESTER")
    res = conn.getresponse()
    assert_test("CGI POST Execution", res.status == 200 and b"BODY:HELLO_FROM_TESTER" in res.read(), "CGI POST failed")
    conn.close()

    # 14. Test HTTP Redirection (301)
    conn = http.client.HTTPConnection("127.0.0.1", 8080)
    conn.request("GET", "/redirect")
    res = conn.getresponse()
    assert_test("HTTP Redirection (301)", res.status == 301 and res.getheader('Location') == '/', f"Redirection failed: {res.status}")
    conn.close()

if __name__ == "__main__":
    print("=========================================")
    print("     WEBSERV ULTIMATE EVAL TESTER        ")
    print("=========================================\n")
    setup_environment()
    
    print("[*] Make sure your webserv is running with: ./webserv ultimate_tester.conf")
    input("Press Enter to begin the automated tests...")
    print()
    
    try:
        run_tests()
    except ConnectionRefusedError:
        print(f"{RED}[FATAL]{RESET} Connection refused! Is the server running?")
    except Exception as e:
        print(f"{RED}[FATAL]{RESET} An unexpected error occurred: {e}")
        
    print("\n[+] To finish evaluation stress tests, run:")
    print("    siege -b http://127.0.0.1:8080/")