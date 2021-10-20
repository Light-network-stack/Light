if [ -z "$LIGHT_SOURCE_PATH" ]
then 
    echo "\$LIGHT_SOURCE_PATH is not set at all. For example, 1) sudo su, and 2) export LIGHT_SOURCE_PATH=/home/light2/"
	exit 1
fi
cd $LIGHT_SOURCE_PATH/Light

if [ $# -ne 1 ]
then
	echo "you need to input one parameter which means the nginx worker number"
	exit 1
fi
if [ $1 -lt 1 -o $1 -gt 16 ]
then 
	echo "number must be in 1 ~ 16, inclusive"
	exit 1
fi

echo "$1 workers will run on $1 cores"
echo "Building the nginx config file ... ..."

export LD_LIBRARY_PATH=/usr/lib/light
echo 0 >/proc/sys/kernel/randomize_va_space
ulimit -n 102400
# cp -f nginx_2worker.conf /usr/local/nginx/conf/nginx.conf
echo "worker_processes  $1;" > ${PWD}/config/nginx_nworker.conf
echo -n "worker_cpu_affinity  " >> ${PWD}/config/nginx_nworker.conf
for num in $( seq 1 $1)
do
	echo -n "1" >> ${PWD}/config/nginx_nworker.conf
done

for num in $( seq 1 $1)
do
	echo -n "0" >> ${PWD}/config/nginx_nworker.conf
done

# coremask for 1 core Light and 1 core Nginx: 100
# echo -n "0" >> ${PWD}/config/nginx_nworker.conf

echo ";" >> ${PWD}/config/nginx_nworker.conf

cat ${PWD}/config/nginx_nworker_basic.conf >> ${PWD}/config/nginx_nworker.conf

# create/overwrite webpage file
# mkdir /srv/www
# mkdir /srv/www/htdocs
# # 64 bytes
# filesize=2048
# echo -n "" > /srv/www/htdocs/index.html
# for num in $( seq 1 $filesize)
# do
# 	echo -n "a" >> /srv/www/htdocs/index.html
# done

echo "Start Nginx ... ..."

# LD_PRELOAD=./light_h6.so /usr/local/nginx/sbin/nginx -c ${PWD}/config/nginx_nworker.conf

# Nginx 1.20.0
LD_PRELOAD=./light_h6.so /usr/local/nginx20/sbin/nginx -c ${PWD}/config/nginx_nworker.conf
