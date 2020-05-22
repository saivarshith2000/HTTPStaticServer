#!/bin/bash
# simple script to test the server

usage() {
    echo "Usage: $0 [-n number of requests] [-p port] [-u url relative to localhost:port/ (Example: benchmark/puppies.html)]"
}

num_requests=100
port=8000
url="/benchmark/puppies/index.html"

while getopts "n:u:p:s:t:" o; do
    case "${o}" in
        n)
            num_requests=${OPTARG}
            ;;
        u)
            url=${OPTARG}
            ;;
        p)
            port=${OPTARG}
            ;;
        s)
            html=${OPTARG}
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done

# make temp directory
rm -rf temp && mkdir temp && cd temp/

test_url="http://localhost:$port/$url"
echo "Benchmark url: $test_url"

# start time
start=`date +%s%N`

# make num_requests
i=0
while [[ $i -le $num_requests ]]
do
    if wget -r -q $test_url > /dev/null; then
        let i=i+1
    else
        echo "wget failed! Make sure you have wget installed and the server is running on $port !"
        exit 1
    fi
done

# end time
end=`date +%s%N`
runtime=$((end-start))

# remove temp
cd ../ && rm -rf temp

# report results
echo "Made $num_requests requests"
echo "Total time: $(expr $runtime / 1000000000) s"
echo "Average time per request: $(expr $(expr $runtime / $num_requests) / 1000000) ms"

exit 0
