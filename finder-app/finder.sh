#!/bin/bash

set -e
set -u

if [ $# -lt 2 ]
then
    echo "Search string parameter is missing. Please enter search string!"

    if [ $# -lt 1 ]
    then
        echo "Directory parameter is missing. Please enter directory parameter!"
    fi

    exit 1
else
    filedir=$1
    searchstr=$2

    if [ -d filedir ]
    then
        echo "Directory does not exist!"
        exit 1
    fi
fi

filecount=$( ls "$filedir" | wc -l )
matchcount=$( grep -r "$searchstr" "$filedir" | wc -l )

echo "The number of files are ${filecount} and the number of matching lines are ${matchcount}"
