#!/bin/sh
# CUPS-filter entrypoint: reads a CUPS raster on stdin, writes SPL3 to stdout.
export LD_LIBRARY_PATH=/opt/uld
export PPD=/opt/10x.ppd
exec /opt/uld/rastertospl 1 user job 1 "${OPTS:-}"
