#!/bin/sh
#
#----------------------------------------------------------------------------#
# Darwin (Mac OS X) client for Hobbit                                        #
#                                                                            #
# Copyright (C) 2005-2006 Henrik Storner <henrik@hswn.dk>                    #
#                                                                            #
# This program is released under the GNU General Public License (GPL),       #
# version 2. See the file "COPYING" for details.                             #
#                                                                            #
#----------------------------------------------------------------------------#
#
# $Id: hobbitclient-darwin.sh,v 1.16 2006-06-10 15:30:26 henrik Exp $

echo "[date]"
date
echo "[uname]"
uname -a
echo "[uptime]"
uptime
echo "[who]"
who
echo "[df]"
# The sed stuff is to make sure lines are not split into two.
df -H -t nonfs,nullfs,cd9660,procfs,volfs,devfs,fdesc | sed -e '/^[^ 	][^ 	]*$/{
N
s/[ 	]*\n[ 	]*/ /
}'
echo "[meminfo]"
vm_stat
echo "[ifconfig]"
ifconfig -a
echo "[route]"
netstat -r
echo "[netstat]"
netstat -s
echo "[ifstat]"
netstat -ibn | egrep -v "^lo|<Link"
echo "[ports]"
netstat -an|grep "^tcp"
echo "[ps]"
ps -ax -ww -o pid,ppid,user,start,state,pri,pcpu,time,pmem,rss,vsz,command

# $TOP must be set, the install utility should do that for us if it exists.
if test "$TOP" != ""
then
    if test -x "$TOP"
    then
        echo "[top]"
	$TOP -l 1 -n 20
    fi
fi

# logfiles
if test -f $LOGFETCHCFG
then
    $BBHOME/bin/logfetch $LOGFETCHCFG $LOGFETCHSTATUS
fi

exit

