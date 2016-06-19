ipcs -m | awk '{ if( NR > 3) print $2}' | head -n -1 | xargs ipcrm shm
