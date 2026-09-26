FROM arm32v7/debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY linux-armv7/needle /opt/needle/needle
COPY needle3.cact /opt/needle/needle3.cact
COPY needle-tools.json /opt/needle/needle-tools.json

RUN chmod 0755 /opt/needle/needle

WORKDIR /opt/needle
ENTRYPOINT ["/opt/needle/needle"]
CMD ["--model", "/opt/needle/needle3.cact", "--tools", "/opt/needle/needle-tools.json", "--serve", "--port", "7101", "--depth", "4", "--threads", "2"]
