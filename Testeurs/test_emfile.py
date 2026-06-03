#!/usr/bin/env python3
import socket
import time

def trigger_fd_exhaustion():
    sockets = []
    print("Opening connections until the server hits its file descriptor limit...")
    
    try:
        # 1050 is usually enough to hit the default ulimit -n of 1024
        for i in range(1050): 
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(('127.0.0.1', 8080))
            sockets.append(s)
            if i > 0 and i % 100 == 0:
                print(f"Opened {i} connections...")
    except Exception as e:
        print(f"Stopped at {len(sockets)} connections. Reason: {e}")

    print("\n---> BUG TRIGGERED! <---")
    print("1. Open a new terminal and run 'top' or 'htop'.")
    print("2. Look at your webserv process. It should be stuck at 100% CPU usage!")
    print("3. Try loading your website in a browser. It will just spin forever.")
    print("\nPress Ctrl+C to close the connections and free the server.")
    
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nClosing sockets...")
        for s in sockets:
            s.close()

if __name__ == "__main__":
    trigger_fd_exhaustion()