#!/usr/bin/env bash
# Default-deny outbound traffic; allow DNS, loopback and the domains in ALLOWED_DOMAINS.
# The npm registry is intentionally absent so rea-agents cannot be fetched inside the clean room.
set -euo pipefail
ALLOWED_DOMAINS="${ALLOWED_DOMAINS:-api.anthropic.com}"

iptables -F OUTPUT
ipset destroy allowed 2>/dev/null || true
ipset create allowed hash:ip

for domain in $ALLOWED_DOMAINS; do
  for ip in $(dig +short A "$domain" | grep -E '^[0-9.]+$'); do
    ipset add allowed "$ip" -exist
  done
done

iptables -A OUTPUT -o lo -j ACCEPT
iptables -A OUTPUT -p udp --dport 53 -j ACCEPT
iptables -A OUTPUT -p tcp --dport 53 -j ACCEPT
iptables -A OUTPUT -m state --state ESTABLISHED,RELATED -j ACCEPT
iptables -A OUTPUT -m set --match-set allowed dst -p tcp --dport 443 -j ACCEPT
iptables -P OUTPUT DROP

# Wall checks: all three must fail.
if command -v rea >/dev/null 2>&1; then echo "rea is on PATH inside the clean room" >&2; exit 1; fi
if curl -s --max-time 5 https://registry.npmjs.org >/dev/null; then echo "npm registry is reachable" >&2; exit 1; fi
echo "clean-room firewall active"
