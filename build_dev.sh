#clean old files
rm -rf stack_and_service/stack_and_service/x86_64-native-linuxapp-gcc
rm -rf stack_and_service/x86_64-native-linuxapp-gcc
rm -rf stack_and_service/service/x86_64-native-linuxapp-gcc
rm -rf stack_and_service/service/light_app_api/x86_64-native-linuxapp-gcc
rm -rf stack_and_service/service/stack_and_service
rm -rf stack_and_service/service/light_app_api/stack_and_service/service
rm -rf stack_and_service/service/log
rm -rf /usr/bin/light_srv
rm -rf /usr/lib/light
rm -rf /usr/include/light_api.h
rm -rf /etc/light
rm -rf /monitor/x86_64-native-linuxapp-gcc
rm monitor/monitor.app

cd dpdk-17.02
# export EXTRA_CFLAGS='-g -O0'
make install T=x86_64-native-linuxapp-gcc
cd ..
#rm -rf build 
ulimit -n 102400
make  CURRENT_DIR=$(pwd)/ clean RTE_SDK=$(pwd)/dpdk-17.02 LD_LIBRARY_PATH='stack_and_service/stack_and_service/x86_64-native-linuxapp-gcc/lib'
make CURRENT_DIR=$(pwd)/ RTE_SDK=$(pwd)/dpdk-17.02 LD_LIBRARY_PATH='stack_and_service/stack_and_service/x86_64-native-linuxapp-gcc/lib'
sudo cp  stack_and_service/service/stack_and_service/service/x86_64-native-linuxapp-gcc/light_srv /usr/bin/.
echo 'install libaries'
sudo mkdir -p /usr/lib/light
sudo cp dpdk-17.02/x86_64-native-linuxapp-gcc/lib/*.so /usr/lib/light/.
sudo cp dpdk-17.02/x86_64-native-linuxapp-gcc/lib/*.so.* /usr/lib/light/.
sudo cp stack_and_service/service/light_app_api/stack_and_service/service/light_app_api/x86_64-native-linuxapp-gcc/lib/liblightservice.so /usr/lib/light/.
sudo cp stack_and_service/stack_and_service/x86_64-native-linuxapp-gcc/lib/libnetinet.so /usr/lib/light/.
echo 'install headers'
sudo cp stack_and_service/service/light_app_api/light_api.h /usr/include/.
sudo mkdir -p /etc/light
sudo cp stack_and_service/service/dpdk_ip_stack_config.txt /etc/light
cp monitor/monitor/x86_64-native-linuxapp-gcc/monitor monitor/monitor.app
./build_light_module.sh
