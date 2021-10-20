# use "sudo" to run this script

if [ $# -ne 1 ]
then
	echo "you need to input one parameter which means the nginx worker number"
	exit 1
fi
if [ $1 -lt 1 -o $1 -gt 32 ]
then 
	echo "number must be in 1 ~ 32, inclusive"
	exit 1
fi

echo "$1 workers will run on $1 cores"
echo "Building nginx config file ... ..."

# export LD_LIBRARY_PATH=/usr/lib/light
echo 0 >/proc/sys/kernel/randomize_va_space
ulimit -n 102400
# cp -f nginx_2worker.conf /usr/local/nginx/conf/nginx.conf
echo "worker_processes  $1;" > ${PWD}/config/nginx_nworker.conf
echo -n "worker_cpu_affinity  " >> ${PWD}/config/nginx_nworker.conf
for num in $( seq 1 $1)
do
	echo -n "1" >> ${PWD}/config/nginx_nworker.conf
done

# for num in $( seq 1 $1)
# do
# 	echo -n "0" >> ${PWD}/config/nginx_nworker.conf
# done

echo ";" >> ${PWD}/config/nginx_nworker.conf
cat ${PWD}/config/nginx_nworker_basic_kernel.conf >> ${PWD}/config/nginx_nworker.conf

# # create/overwrite webpage file
# mkdir /srv/www
# mkdir /srv/www/htdocs
# # 64 bytes
# filesize=64
# echo -n "" > /srv/www/htdocs/index.html
# for num in $( seq 1 $filesize)
# do
# 	echo -n "a" >> /srv/www/htdocs/index.html
# done

echo "Start Nginx ... ..."

# show Nginx version
# /usr/local/nginx20/sbin/nginx -v

# run Nginx with custom config file
# /usr/local/nginx/sbin/nginx -c ${PWD}/config/nginx_nworker.conf
# Nginx 1.20.0
/usr/local/nginx20/sbin/nginx -c ${PWD}/config/nginx_nworker.conf

# install nginx by apt
# sudo apt install nginx

# check the current used config file
# sudo nginx -t

# Check status
# systemctl status nginx

# Reload Nginx (after updating the config)
# sudo systemctl reload nginx

# Stop Nginx
# sudo systemctl stop nginx

# 查找Nginx配置文件: #find / -name nginx.conf
# 查看Nginx进程号: #pgrep nginx
# 	或 #ps -ef | grep nginx
# 杀死进程：#kill -9 <PID>
# 杀死Nginx进程（仅需一步）: #pkill -9 nginx
# 杀死Nginx进程: #pgrep nginx | xargs kill -9

# monitor the speed of every NIC:
# sudo apt-get install bwm-ng
# bwm-ng

# 查看并发连接数：
# netstat -na|grep ESTABLISHED|wc -l

# 解除fd数目限制(should be set on both the client and server side):
# ulimit -n 102400 

