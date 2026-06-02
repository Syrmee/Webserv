#!/usr/bin/env python3
import os
import sys
import http.client
import time
import socket
import subprocess

# ANSI Colors
GREEN = '\033[92m'
YELLOW = '\033[93m'
RED = '\033[91m'
BLUE = '\033[94m'
RESET = '\033[0m'

TARGET_HOST = "127.0.0.1"
TARGET_PORT = 8080
WEBSERV_PID = None

def get_webserv_pid():
    """Attempts to find the PID of the running webserv process for memory tracking."""
    try:
        pid = subprocess.check_output(["pgrep", "webserv"]).decode().strip().split('\n')[0]
        return int(pid)
    except Exception:
        return None

def get_memory_usage(pid):
    """Returns Resident Set Size (RSS) memory of the process in KB."""
    if not pid:
        return 0
    try:
        out = subprocess.check_output(["ps", "-p", str(pid), "-o", "rss="]).decode().strip()
        return int(out)
    except Exception:
        return 0

def setup_fixtures():
    print(f"{BLUE}[*] Setting up advanced test filesystem in /tmp/www...{RESET}")
    dirs = [
        "/tmp/www",
        "/tmp/www/upload",
        "/tmp/www/auto",
        "/tmp/www/index_test",
        "/tmp/www/cgi-bin"
    ]
    for d in dirs:
        os.makedirs(d, exist_ok=True)
        
    with open("/tmp/www/custom_404.html", "w") as f:
        f.write("CUSTOM_404_PAGE_CONTENT")
        
    with open("/tmp/www/index_test/my_index.html", "w") as f:
        f.write("INDEX_DIR_CONTENT")
        
    with open("/tmp/www/auto/auto_file.txt", "w") as f:
        f.write("AUTOINDEX_TEST")

    with open("/tmp/www/empty.html", "w") as f:
        f.write("") # Required empty file for the mandatory siege test
        
    # Create an Infinite Loop CGI Script to test Issue #4 (CGI Timeout)
    cgi_loop_path = "/tmp/www/cgi-bin/infinite_loop.py"
    with open(cgi_loop_path, "w") as f:
        f.write("#!/usr/bin/env python3\n")
        f.write("import time\n")
        f.write("print('Content-Type: text/html\\r\\n\\r\\n', end='')\n")
        f.write("while True:\n")
        f.write("    time.sleep(1)\n")
    os.chmod(cgi_loop_path, 0o755)

    # Create a normal working CGI script to test GET/POST environment variables (Issue #6)
    cgi_env_path = "/tmp/www/cgi-bin/env_test.py"
    with open(cgi_env_path, "w") as f:
        f.write("#!/usr/bin/env python3\n")
        f.write("import os, sys\n")
        f.write("print('Content-Type: text/plain\\r\\n\\r\\n', end='')\n")
        f.write("print(f\"METHOD={os.environ.get('REQUEST_METHOD', '')}\")\n")
        f.write("print(f\"PORT={os.environ.get('SERVER_PORT', '')}\")\n")
        if os.environ.get('REQUEST_METHOD') == 'POST':
            print("body = sys.stdin.read()\n", file=f)
            print("print(f\"BODY={body}\")\n", file=f)
    os.chmod(cgi_env_path, 0o755)


def run_functional_test(name, expected_status, method, path, headers={}, body=None, expect_in_body=None):
    print(f"Testing Functional: {name.ljust(50)}", end="")
    try:
        conn = http.client.HTTPConnection(TARGET_HOST, TARGET_PORT, timeout=5)
        conn.request(method, path, body=body, headers=headers)
        response = conn.getresponse()
        status = response.status
        res_body = response.read().decode('utf-8', errors='ignore')
        conn.close()
        
        if status == expected_status:
            if expect_in_body and expect_in_body not in res_body:
                print(f"[{RED}NO (Body Mismatch){RESET}]")
                print(f"    Expected substring: '{expect_in_body}'")
            else:
                print(f"[{GREEN}OK{RESET}]")
        else:
            print(f"[{RED}NO{RESET}] -> Expected {expected_status}, Got {status}")
    except Exception as e:
        print(f"[{RED}CRASH/ERROR{RESET}] -> {e}")

def test_chunked_request():
    print(f"Testing Functional: Chunked Request Un-chunking (POST)".ljust(70), end="")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((TARGET_HOST, TARGET_PORT))
        
        # Manually assemble raw chunked payload to verify parser state machine
        request_raw = (
            "POST /upload/chunked_file.txt HTTP/1.1\r\n"
            f"Host: {TARGET_HOST}:{TARGET_PORT}\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "4\r\nWiki\r\n"
            "5\r\npedia\r\n"
            "E\r\n in\r\nchunks.\r\n"
            "0\r\n\r\n"
        )
        s.sendall(request_raw.encode())
        response = s.recv(4096).decode()
        s.close()
        
        if "201 Created" in response:
            print(f"[{GREEN}OK{RESET}]")
        else:
            print(f"[{RED}NO{RESET}] -> Unexpected response for chunked data.")
    except Exception as e:
        print(f"[{RED}ERROR{RESET}] -> {e}")

def test_client_activity_timeout():
    """Simulates a slow client connection hanging (Issue #5) to confirm disconnection."""
    print(f"Testing Timeout: Client Idle Disconnection (Slowloris Protection)".ljust(70), end="")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(15)
        s.connect((TARGET_HOST, TARGET_PORT))
        
        # Send partial headers and hold the connection open
        s.send(b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n")
        
        start_time = time.time()
        # Keep waiting until server cuts the connection or 12 seconds pass
        while time.time() - start_time < 12:
            data = s.recv(1024)
            if not data: # Server dropped connection (FIN packet sent)
                duration = time.time() - start_time
                print(f"[{GREEN}OK{RESET}] dropped gracefully after {duration:.2f}s")
                s.close()
                return
            time.sleep(0.5)
            
        print(f"[{RED}NO{RESET}] -> Server kept connection hanging indefinitely.")
        s.close()
    except socket.error:
        print(f"[{GREEN}OK{RESET}] dropped immediately via socket break")

def test_cgi_infinite_loop_timeout():
    """Ensures CGI scripts running infinite loops break out cleanly with a 504 Gateway Timeout (Issue #4)."""
    print(f"Testing Timeout: CGI Infinite Loop (504 Gateway Timeout Verification)".ljust(70), end="")
    try:
        conn = http.client.HTTPConnection(TARGET_HOST, TARGET_PORT, timeout=15)
        conn.request("GET", "/cgi-bin/infinite_loop.py")
        response = conn.getresponse()
        status = response.status
        conn.close()
        
        if status == 504:
            print(f"[{GREEN}OK{RESET}]")
        else:
            print(f"[{RED}NO{RESET}] -> Expected 504 Gateway Timeout, got {status}")
    except Exception as e:
        print(f"[{RED}NO{RESET}] -> Handled incorrectly or crashed: {e}")

def run_stress_test():
    """Executes the mandatory evaluation benchmarks via siege tool."""
    global WEBSERV_PID
    print(f"\n{BLUE}--- STARTING STRESS & AVAILABILITY BENCHMARKS ---{RESET}")
    
    WEBSERV_PID = get_webserv_pid()
    if not WEBSERV_PID:
        print(f"{YELLOW}[!] Warning: webserv process not explicitly found. Skipping internal RSS leak checks.{RESET}")
    
    mem_before = get_memory_usage(WEBSERV_PID)
    if mem_before:
        print(f"[*] Initial Server Memory usage: {mem_before} KB")

    # Command parameters extracted directly from evaluation criteria sheet requirements
    siege_cmd = ["siege", "-b", "-c", "50", "-t", "10S", f"http://{TARGET_HOST}:{TARGET_PORT}/empty.html"]
    
    print(f"[*] Spawning Siege: {' '.join(siege_cmd)}")
    print("[*] Running brute stress verification on an empty page...")
    
    try:
        # Run siege and capture stderr output where statistical charts are printed
        proc = subprocess.Popen(siege_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = proc.communicate()
        siege_output = stderr.decode()
        
        print("\n--- SIEGE RESULTS ---")
        availability = None
        for line in siege_output.split('\n'):
            if "Availability" in line or "Availability" in line:
                print(f"{GREEN}{line}{RESET}")
                # Parse out the percentage float values safely
                try: availability = float(line.split()[1].replace('%', ''))
                except ValueError: pass
            elif "Failed responses" in line or "Longest transaction" in line:
                print(line)
        print("---------------------\n")
        
        if availability and availability >= 99.5:
            print(f"Availability Validation: >= 99.5% requirement met [{GREEN}PASSED{RESET}]")
        else:
            print(f"Availability Validation: Under target benchmark [{RED}FAILED{RESET}]")
            
        mem_after = get_memory_usage(WEBSERV_PID)
        if mem_after and mem_before:
            mem_diff = mem_after - mem_before
            print(f"[*] Final Server Memory usage: {mem_after} KB (Delta: {mem_diff} KB)")
            if mem_diff > 2048: # Flag an issue if memory bloated up by more than 2MB over a 10s test
                print(f"{RED}[!] Leak Alert: Process memory increased noticeably under load!{RESET}")
            else:
                print(f"Memory Leak Monitoring Validation: [{GREEN}PASSED{RESET}]")
                
    except FileNotFoundError:
        print(f"{RED}[X] Error: 'siege' command-line utility is missing. Run 'brew install siege' or 'sudo apt install siege'.{RESET}")

if __name__ == "__main__":
    setup_fixtures()
    time.sleep(0.5)

    print(f"\n{BLUE}==================================================={RESET}")
    print(f"{BLUE}      WEBSERV FUNCTIONAL & COMPLIANCE SUITE        {RESET}")
    print(f"{BLUE}==================================================={RESET}\n")

    # Standard evaluations
    run_functional_test("Basic Home Route Serving File (GET)", 200, "GET", "/index_test/my_index.html", expect_in_body="INDEX_DIR_CONTENT")
    run_functional_test("Custom Configured Page Replacement (404 Error)", 404, "GET", "/non_existent_resource", expect_in_body="CUSTOM_404_PAGE_CONTENT")
    run_functional_test("Route Rule Method Limitation Verification (DELETE)", 405, "DELETE", "/index_test/my_index.html")
    run_functional_test("Directory Configuration Index Routing", 301, "GET", "/index_test") # checks trailing slash resolution logic
    run_functional_test("Autoindex Generation Matrix Check", 200, "GET", "/auto/", expect_in_body="auto_file.txt")
    
    # Advanced standard protocols checks
    test_chunked_request()
    
    # Advanced CGI Environment & Transmission Assertions (Issue #6 Verification)
    run_functional_test("CGI Script Header Verification (GET Method Context)", 200, "GET", "/cgi-bin/env_test.py", expect_in_body="METHOD=GET")
    
    print(f"\n{BLUE}--- STARTING TIMEOUT RESILIENCE SESSIONS ---{RESET}\n")
    test_client_activity_timeout()
    test_cgi_infinite_loop_timeout()
    
    # Mandatory Evaluation Load Benchmark
    run_stress_test()