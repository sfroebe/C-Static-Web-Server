#!/bin/bash

# Check if the script is running as root
if [ "$EUID" -ne 0 ]; then
  echo "This script requires root privileges. Relaunching with sudo..."
  exec sudo "$0" "$@"
fi

REQUESTS=${1:-1000}
CONCURRENCY=${2:-10}

RESULTS_DIR="benchmark/results"

mkdir -p "$RESULTS_DIR"

echo
echo "Benchmark Configuration"
echo "-----------------------"
echo "Requests   : $REQUESTS"
echo "Concurrency: $CONCURRENCY"
echo

echo
echo "========================================"
echo "Copying website into Apache/NGINX web root..."
echo "========================================"

rm -rf /var/www/html/*
cp -r public/* /var/www/html/

############################################################
# Benchmark Custom C Server
############################################################

echo
echo "========================================"
echo "Benchmarking Custom C Server"
echo "========================================"

./server &
SERVER_PID=$!

sleep 2

ab -n $REQUESTS -c $CONCURRENCY http://127.0.0.1:8080/ \
| tee "$RESULTS_DIR/custom.txt"

kill $SERVER_PID

sleep 2

############################################################
# Benchmark NGINX
############################################################

echo
echo "========================================"
echo "Benchmarking NGINX"
echo "========================================"

systemctl stop apache2
systemctl start nginx

sleep 2

ab -n $REQUESTS -c $CONCURRENCY http://127.0.0.1/ \
| tee "$RESULTS_DIR/nginx.txt"

############################################################
# Benchmark Apache
############################################################

echo
echo "========================================"
echo "Benchmarking Apache"
echo "========================================"

systemctl stop nginx
systemctl start apache2

sleep 2

ab -n $REQUESTS -c $CONCURRENCY http://127.0.0.1/ \
| tee "$RESULTS_DIR/apache.txt"

############################################################
# Summary
############################################################

echo
echo "========================================"
echo "Benchmark Summary"
echo "========================================"

for file in "$RESULTS_DIR"/*.txt
do
    echo
    echo "Results from: $file"

    grep "Requests per second" "$file"
    grep "Time per request" "$file" | head -1
    grep "Failed requests" "$file"
done

echo
echo "========================================"
echo "Benchmark Complete!"
echo "========================================"