#!/usr/bin/env python3
import http.client
import socket
import time
import subprocess
import os

def check(name, status, expected):
    ok = "OK" if status == expected else f"FAIL (got {status})"
    print(f"  {name}: {ok}")

def check_alive():
    try:
        c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=2)
        c.request("GET", "/empty.html")
        r = c.getresponse()
        c.close()
        return r.status == 200
    except:
        return False

# Setup
os.makedirs("/tmp/www/upload", exist_ok=True)
os.makedirs("/tmp/www/auto", exist_ok=True)
os.makedirs("/tmp/www/index_test", exist_ok=True)
os.makedirs("/tmp/www/cgi-bin", exist_ok=True)
with open("/tmp/www/empty.html", "w") as f: f.write("")
with open("/tmp/www/custom_404.html", "w") as f: f.write("CUSTOM_404")
with open("/tmp/www/index_test/my_index.html", "w") as f: f.write("INDEX")
with open("/tmp/www/auto/file.txt", "w") as f: f.write("test")

print("=== QUICK DIAGNOSTIC ===")
print(f"Server alive: {check_alive()}")

# Basic tests
print("\n--- Functional Tests ---")
for name, method, path, expected in [
    ("GET file", "GET", "/index_test/my_index.html", 200),
    ("GET 404", "GET", "/nonexistent", 404),
    ("DELETE 405", "DELETE", "/index_test/my_index.html", 405),
    ("GET redirect", "GET", "/index_test", 301),
    ("GET autoindex", "GET", "/auto/", 200),
]:
    try:
        c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=2)
        c.request(method, path)
        r = c.getresponse()
        r.read()
        check(name, r.status, expected)
        c.close()
    except Exception as e:
        print(f"  {name}: CRASH - {e}")
        print(f"  Server alive after: {check_alive()}")
        break

print(f"\nServer alive after functional: {check_alive()}")

# Siege test
print("\n--- Siege Test ---")
pid = None
try:
    pid = int(subprocess.check_output(["pgrep", "webserv"]).decode().strip().split('\n')[0])
    mem_before = int(subprocess.check_output(["ps", "-p", str(pid), "-o", "rss="]).decode().strip())
    print(f"Memory before: {mem_before} KB")
except: 
    print("Could not get pid/memory")

proc = subprocess.Popen(
    ["siege", "-b", "-c", "50", "-t", "10S", "http://127.0.0.1:8080/empty.html"],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE
)
stdout, stderr = proc.communicate()
output = stderr.decode()
for line in output.split('\n'):
    if 'Availability' in line or 'Failed' in line or 'Transaction' in line:
        print(f"  {line.strip()}")

if pid:
    try:
        mem_after = int(subprocess.check_output(["ps", "-p", str(pid), "-o", "rss="]).decode().strip())
        print(f"Memory after: {mem_after} KB (delta: {mem_after - mem_before} KB)")
    except:
        print("Server died during siege!")

print(f"\nServer alive after siege: {check_alive()}")
