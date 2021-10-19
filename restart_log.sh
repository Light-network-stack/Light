#!/bin/bash
rm -rf /var/log/light
mkdir /var/log/light
chown -R syslog:adm /var/log/light
service rsyslog restart
service syslog restart
