# Runs HP's real `rastertospl` (from the Unified Linux Driver) inside a tiny Linux
# container so it works on macOS. The binary + vendor lib + PPD are copied in by
# install.sh (they are NOT redistributed in this repo - install.sh fetches the ULD
# straight from HP).
FROM ubuntu:22.04
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends libcups2 libcupsimage2 \
 && rm -rf /var/lib/apt/lists/*
COPY uld/ /opt/uld/
COPY 10x.ppd /opt/10x.ppd
COPY run.sh /run.sh
RUN chmod +x /run.sh /opt/uld/rastertospl
ENTRYPOINT ["/run.sh"]
