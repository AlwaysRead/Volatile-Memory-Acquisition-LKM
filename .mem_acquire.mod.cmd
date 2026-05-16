savedcmd_mem_acquire.mod := printf '%s\n'   mem_acquire.o | awk '!x[$$0]++ { print("./"$$0) }' > mem_acquire.mod
