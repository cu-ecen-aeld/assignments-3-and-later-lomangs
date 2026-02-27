#!/bin/bash

set -e
set -u

if [ $# -lt 2 ]
then
    echo "The string parameter to copy is missing. Please enter a string parameter to copy!"

    if [ $# -lt 1 ]
    then
        echo "Please enter a file name to create!"
    fi

    exit 1
else    
    writefile=$1
    writestr=$2
fi  

if [ ! -d $(dirname "$writefile") ]
then
    mkdir -p $(dirname "$writefile")
fi

echo "write directory: $writefile"
echo "$writestr" > "$writefile"
