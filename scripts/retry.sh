#!/bin/bash
max_attempts=10
timeout=2
attempt=1
exitCode=0

while (( attempt <= max_attempts ))
do
  "$@"
  exitCode=$?

  if [[ $exitCode == 0 ]]
  then
    break
  fi

  echo "Command failed with exit code $exitCode. Retrying in $timeout seconds..." 1>&2
  sleep $timeout
  attempt=$(( attempt + 1 ))
  timeout=$(( timeout + 2 ))
done

if [[ $exitCode != 0 ]]
then
  echo "Command failed after $max_attempts attempts." 1>&2
fi

exit $exitCode
