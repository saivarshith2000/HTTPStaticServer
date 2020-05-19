#!/bin/bash

# simple bash script to test the server
max_requests=1000
i=0

# if user passed a port number and/or url
port=8000
url=""

if [ $# -eq 1 ]; then
    port=$1
fi
if [ $# -eq 2 ]; then
    port=$1
    url=$2
fi

# start time
start=`date +%s%N`

# make max_requests
while [[ $i -le $max_requests ]]
do
    if curl -s "http://localhost:$port/" > /dev/null; then
        let i=i+1
    else
        echo "Curl failed! Make sure you have curl installed and the server is running on $port !"
        exit
    fi
done
# end time
end=`date +%s%N`
runtime=$((end-start))

echo "Made $max_requests requests to http://localhost:$port/$url"
echo "Total time: $(expr $runtime / 1000000000) s"
echo "Average time per request: $(expr $(expr $runtime / $max_requests) / 100000) ms"
exit
