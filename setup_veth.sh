#!/bin/bash
set -e

echo "[*] Cleaning up any old virtual cable..."
sudo ip link del veth-exch 2>/dev/null || true

echo "[*] Creating virtual network cable: veth-exch <---> veth-zt"
sudo ip link add veth-exch type veth peer name veth-zt

# Set fixed MAC addresses so our raw packets always know where to go
sudo ip link set dev veth-exch address 02:00:00:00:00:01
sudo ip link set dev veth-zt   address 02:00:00:00:00:02

# Bring both ends of the cable UP
sudo ip link set veth-exch up
sudo ip link set veth-zt up

# Disable packet coalescing for minimum latency
sudo ethtool -K veth-exch tx off rx off 2>/dev/null || true
sudo ethtool -K veth-zt   tx off rx off 2>/dev/null || true

echo "[+] Success! Virtual cable is LIVE:"
ip link show veth-exch
ip link show veth-zt
