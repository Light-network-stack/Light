FLAGS=-Ofast
# FLAGS=-O0
#FLAGS=-g
#gcc $FLAGS -c light_module.c -o light_module.o
#gcc light_module.o -L/usr/lib/light -lrt -ldl -llightservice -lrte_eal -lrte_ring -lrte_timer -lrte_mempool -lrte_malloc -lrte_mbuf -lrt -ldl -pthread -o light_module.bin

rm -rf light_h6.o light_h6.so
gcc $FLAGS -c light_h.c -o light_h6.o
gcc light_h.c -L/usr/lib/light -lrt -ldl -llightservice -lrte_eal -lrte_ring -lrte_timer -lrte_mempool -lrte_mbuf -lrt -ldl -pthread  -fPIC -shared -o light_h6.so
