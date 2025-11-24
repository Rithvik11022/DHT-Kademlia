#!/usr/bin/env bash
set -euo pipefail

BIN=./bin/kademlia
PORT=4000

if [ ! -x "$BIN" ]; then
  echo "Binary $BIN not found. Run 'make' first."
  exit 2
fi

# Generate a random 40-hex key (160-bit)
KEY=$(xxd -l 20 -p /dev/urandom)
VALUE="hello-test-$(date +%s)"

echo "Starting single node on 127.0.0.1:$PORT"
# Run node and send commands via here-doc. The node exits after EOF.
# Node prints id at startup; we capture it for debugging but the script won't parse it.
printf "STORE %s %s\nFINDVAL %s\nQUIT\n" "$KEY" "$VALUE" "$KEY" | "$BIN" --port "$PORT"
echo "Script finished. If you saw 'VALUE: $VALUE' above, test passed."
