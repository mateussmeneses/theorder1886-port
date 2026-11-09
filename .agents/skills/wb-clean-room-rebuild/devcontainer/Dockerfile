# Clean-room image: Claude Code and build tools, no REA, outbound traffic limited by init-firewall.sh.
FROM node:22-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      iptables ipset dnsutils sudo git build-essential python3 \
    && rm -rf /var/lib/apt/lists/*
# Install project dependencies here, before the firewall removes registry access.
RUN npm install -g @anthropic-ai/claude-code
COPY init-firewall.sh /usr/local/bin/init-firewall.sh
RUN chmod +x /usr/local/bin/init-firewall.sh \
    && echo "node ALL=(root) NOPASSWD: /usr/local/bin/init-firewall.sh" > /etc/sudoers.d/firewall
USER node
