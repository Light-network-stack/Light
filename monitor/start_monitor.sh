export LD_LIBRARY_PATH=/usr/lib/light
echo 0 >/proc/sys/kernel/randomize_va_space
./monitor.app -c 100 -n 4 -d librte_pmd_ixgbe.so  --proc-type=secondary  -- -p 0x3
