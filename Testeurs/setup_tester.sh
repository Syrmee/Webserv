#!/bin/bash

BASE_DIR="/tmp/www/YoupiBanane"

echo "Creating testing directories in $BASE_DIR..."
mkdir -p "$BASE_DIR/nop"
mkdir -p "$BASE_DIR/Yeah"
mkdir -p "/tmp/www/cgi-bin"

echo "Creating index.html for root directory..."
echo "<html><body><h1>Hello from Webserv!</h1></body></html>" > /tmp/www/index.html

echo "Creating dummy files..."
touch "$BASE_DIR/youpi.bad_extension"
touch "$BASE_DIR/youpi.bla"
touch "$BASE_DIR/nop/youpi.bad_extension"
touch "$BASE_DIR/nop/other.pouic"
touch "$BASE_DIR/Yeah/not_happy.bad_extension"

cp cgi_test /tmp/www/cgi-bin

echo "Setup complete! You can now run the tester."
