#!/usr/bin/env python3
import os
import http.client
import time

# ANSI Colors
GREEN = '\033[92m'
RED = '\033[91m'
RESET = '\033[0m'

def setup_fixtures():
    print("Setting up test filesystem in /tmp/www...")
    dirs = [
        "/tmp/www",
        "/tmp/www/upload",
        "/tmp/www/auto",
        "/tmp/www/index_dir",
        "/tmp/www/port2",
        "/tmp/www/host2"
    ]
    for d in dirs:
        os.makedirs(d, exist_ok=True)
        
    with open("/tmp/www/custom_404.html", "w") as f:
        f.write("CUSTOM_404_PAGE_CONTENT")
        
    with open("/tmp/www/index_dir/my_index.html", "w") as f:
        f.write("INDEX_DIR_CONTENT")
        
    with open("/tmp/www/auto/auto_file.txt", "w") as f:
        f.write("AUTOINDEX_TEST")
        
    with open("/tmp/www/port2/index.html", "w") as f:
        f.write("PORT2_CONTENT")
        
    with open("/tmp/www/host2/index.html", "w") as f:
        f.write("HOST2_CONTENT")

def send_request(method, path, headers={}, body=None, host="127.0.0.1", port=8080):
    try:
        conn = http.client.HTTPConnection(host, port, timeout=2)
        conn.request(method, path, body=body, headers=headers)
        response = conn.getresponse()
        status = response.status
        res_body = response.read().decode('utf-8', errors='ignore')
        conn.close()
        return status, res_body
    except Exception as e:
        return 0, str(e)

def test(name, expected_status, method, path, headers={}, body=None, host="127.0.0.1", port=8080, expect_in_body=None):
    print(f"Testing: {name.ljust(50)}", end="")
    status, res_body = send_request(method, path, headers, body, host, port)
    
    if status == expected_status:
        if expect_in_body and expect_in_body not in res_body:
            print(f"[{RED}NO{RESET}] -> Status OK, but body mismatch!")
            print(f"    Expected to find: '{expect_in_body}'")
        else:
            print(f"[{GREEN}OK{RESET}]")
    else:
        print(f"[{RED}NO{RESET}] -> Expected {expected_status}, Got {status}")
        if status == 0:
            print(f"    Error: {res_body}")

if __name__ == "__main__":
    setup_fixtures()
    time.sleep(0.5) # Give filesystem a moment

    print(f"\n--- STARTING WEBSERVER EVALUATION TESTS ---\n")

    # 1. Multiple Ports & Hosts
    test("Multiple Ports (127.0.0.1:8081)", 200, "GET", "/", port=8081, expect_in_body="PORT2_CONTENT")
    test("Multiple Hosts (127.0.0.2:8080)", 200, "GET", "/", host="127.0.0.2", port=8080, expect_in_body="HOST2_CONTENT")

    # 2. Error Page & Unknown Requests
    test("Default Error Page (404)", 404, "GET", "/invalid_path", expect_in_body="CUSTOM_404_PAGE_CONTENT")
    test("UNKNOWN Method", 501, "MAGIC", "/")

    # 3. Client Body Limits
    small_body = "A" * 10
    large_body = "A" * 100
    test("Body Under Limit (10 bytes)", 201, "POST", "/limit/small.txt", body=small_body)
    test("Body Over Limit (100 bytes)", 413, "POST", "/limit/large.txt", body=large_body)

    # 4. Directories & Indexes
    test("Routes to different directories", 200, "GET", "/index_test/", expect_in_body="INDEX_DIR_CONTENT")
    test("Autoindex Directory Listing", 200, "GET", "/auto/", expect_in_body="auto_file.txt")

    # 5. Method Restrictions
    test("Method Rejected (DELETE on /)", 405, "DELETE", "/")

    # 6. Upload (POST) -> GET -> DELETE
    upload_data = "Hello from the test script!"
    test("Upload File (POST)", 201, "POST", "/upload/test_file.txt", body=upload_data)
    test("Retrieve File (GET)", 200, "GET", "/upload/test_file.txt", expect_in_body=upload_data)
    test("Delete File (DELETE)", 204, "DELETE", "/upload/test_file.txt")
    test("Verify Deletion (GET -> 404)", 404, "GET", "/upload/test_file.txt")

    # 7. Redirection
    test("Redirection (301)", 301, "GET", "/redir")

    print("\n--- TESTS FINISHED ---")
