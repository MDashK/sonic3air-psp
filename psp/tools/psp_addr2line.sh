#!/bin/bash
# Function names for code addresses of the PRX (as in allocs.txt or an exception dump), with the build's ELF
#   ./psp_addr2line.sh 0x8bf739c ...      (the PRX is loaded at 0x08804000)
E=~/s3air-psp/Oxygen/sonic3air/build/_psp/build/sonic3air
for a in "$@"; do
  o=$(printf "0x%x" $((a - 0x08804000)))
  echo "$a: $(~/pspdev/bin/psp-addr2line -f -C -e $E $o | head -1 | cut -c1-150)"
done
