#!/usr/bin/env bash
# Requires tmux installed. Starts 3 panes with nodes on ports 3000,3001,3002
SESSION="kademlia-demo-session"
BIN="./bin/kademlia"

if [ ! -x "$BIN" ]; then
  echo "Build first: make"
  exit 1
fi

tmux new-session -d -s "$SESSION" "$BIN --port 3000"
# split window for node2
tmux split-window -h -t "$SESSION"
tmux send-keys -t "$SESSION":0.1 "$BIN --port 3001 --bootstrap 127.0.0.1:3000" C-m
# split window for node3 below node1
tmux select-pane -t "$SESSION":0.0
tmux split-window -v -t "$SESSION"
tmux send-keys -t "$SESSION":0.2 "$BIN --port 3002 --bootstrap 127.0.0.1:3000" C-m

echo "tmux session '$SESSION' started with 3 nodes."
echo "Attach with: tmux attach -t $SESSION"
