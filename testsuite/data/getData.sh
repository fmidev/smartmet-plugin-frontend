#! /bin/sh

# This script is used to get test data for the tests in testsuite

set -x
server="smartmet.fmi.fi"
backends=$(curl "http://$server/info?what=backends&format=ascii")
dest=$(dirname "$0")
cnt=0
for backend in $backends; do
  echo "Getting data for backend: $backend"
  cnt=$((cnt+1))
  curl "http://$server/$backend/info?what=qengine&format=json&timeformat=iso" | jq . >"$dest"/q$(printf "%02d" $cnt).json
  curl "http://$server/$backend/info?what=gridgenerations&format=json&timeformat=iso" | jq . >"$dest"/gg$(printf "%02d" $cnt).json
  curl "http://$server/$backend/info?what=gridgenerationsqd&format=json&timeformat=iso" | jq . >"$dest"/gq$(printf "%02d" $cnt).json
done
echo "Got data for $cnt backends."
# End of script
