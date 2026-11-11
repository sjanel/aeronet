#!/bin/bash
# Runs a command until it succeeds, at most RETRY_ATTEMPTS times (default 10). Lower it for a command that
# also compiles, so that a build error is not repeated 10 times.
# If RETRY_TIMEOUT is set (in seconds), each attempt is killed after that delay, so that a stalled
# download (FetchContent, git clone...) is retried instead of freezing the job.
max_attempts=${RETRY_ATTEMPTS:-10}
delay=2
attempt=1
exitCode=0

while (( attempt <= max_attempts ))
do
  if [[ -n ${RETRY_TIMEOUT:-} ]] && command -v timeout >/dev/null
  then
    timeout "$RETRY_TIMEOUT" "$@"
  else
    "$@"
  fi
  exitCode=$?

  if [[ $exitCode == 0 ]] || (( attempt == max_attempts ))
  then
    break
  fi

  echo "Command failed with exit code $exitCode. Retrying in $delay seconds..." 1>&2
  sleep $delay
  attempt=$(( attempt + 1 ))
  delay=$(( delay + 2 ))
done

if [[ $exitCode != 0 ]]
then
  echo "Command failed after $max_attempts attempts." 1>&2
fi

exit $exitCode
