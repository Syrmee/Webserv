#!/bin/bash

BASE_DIR="/home/londardi/webserv_testing/YoupiBanane"

echo "Creating testing directories in $BASE_DIR..."
mkdir -p "$BASE_DIR/nop"
mkdir -p "$BASE_DIR/Yeah"

echo "Creating index.html for root directory..."
echo "<html><body><h1>Hello from Webserv!</h1></body></html>" > /home/londardi/webserv_testing/index.html

echo "Creating dummy files..."
touch "$BASE_DIR/youpi.bad_extension"
touch "$BASE_DIR/youpi.bla"
touch "$BASE_DIR/nop/youpi.bad_extension"
touch "$BASE_DIR/nop/other.pouic"
touch "$BASE_DIR/Yeah/not_happy.bad_extension"

echo "Checking for cgi_test executable in the project folder..."
if [ -f "/home/londardi/Desktop/Webserv-20260610T144940Z-3-001/Webserv/cgi_test" ]; then
    chmod +x "/home/londardi/Desktop/Webserv-20260610T144940Z-3-001/Webserv/cgi_test"
    echo "cgi_test found and marked as executable!"
else
    echo "Warning: cgi_test executable not found in your project folder. Make sure to download it."
fi

echo "Setup complete! You can now run the tester."