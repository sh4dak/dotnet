#!/bin/sh
COMMAND=/usr/local/bin/dotnet
# To make ports exposeable
# Note: $DATA_DIR is defined in /etc/profile
DEFAULT_ARGS=" --datadir=$DATA_DIR --reseed.verify=true --upnp.enabled=false --http.enabled=true --http.address=0.0.0.0 --httpproxy.enabled=true --httpproxy.address=0.0.0.0 --socksproxy.enabled=true --socksproxy.address=0.0.0.0 --sam.enabled=true --sam.address=0.0.0.0"

if [ "$1" = "--help" ]; then
    set -- $COMMAND --help
else
    ln -s /dotnet_certificates "$DATA_DIR"/certificates
    set -- $COMMAND $DEFAULT_ARGS $@
fi

exec "$@"
