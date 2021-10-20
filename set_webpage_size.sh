# please run with sudo
if [ $# -ne 1 ]
then
	echo "you need to input one parameter which means the size of webpage. E.g. 64 means 64 bytes."
	exit 1
fi

# create/overwrite webpage file
mkdir /srv/www
mkdir /srv/www/htdocs
# 64 bytes
filesize=$1
echo -n "" > /srv/www/htdocs/index.html
for num in $( seq 1 $filesize)
do
	echo -n "a" >> /srv/www/htdocs/index.html
done
stat /srv/www/htdocs/index.html