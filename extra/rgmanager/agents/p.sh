#!/bin/bash

#
# Dummy OCF script for resource group
#
#
# Copyright (C) 1997-2003 Sistina Software, Inc.  All rights reserved.
# Copyright (C) 2004-2011 Red Hat, Inc.  All rights reserved.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#

meta_data()
{
    cat <<EOT
<?xml version="1.0"?>
<resource-agent version="rgmanager 2.0" name="p">
    <version>1.0</version>

    <longdesc lang="en">
        This defines a collection of resources, known as a resource
        group or cluster service.
    </longdesc>
    <shortdesc lang="en">
        Defines a service (resource group).
    </shortdesc>

    <parameters>
        <parameter name="name" unique="1" required="1" primary="1">
            <longdesc lang="en">
                This is the name of the resource group.
            </longdesc>
            <shortdesc lang="en">
                Name.
            </shortdesc>
            <content type="string"/>
        </parameter>

	<parameter name="nfs_client_cache">
            <longdesc lang="en">
	   	On systems with large numbers of exports, a performance
		problem in the exportfs command can cause inordinately long
		status check times for services with lots of mounted
		NFS clients.  This occurs because exportfs does DNS queries
		on all clients in the export list.

		Setting this option to '1' will enable caching of the export
		list returned from the exportfs command on a per-service
		basis.  The cache will last for 30 seconds before expiring
		instead of being generated each time an nfsclient resource
		is called.
            </longdesc>
            <shortdesc lang="en">
	    	Enable exportfs list caching (performance).
            </shortdesc>
	    <content type="integer" default="0"/>
	</parameter>

    </parameters>

    <actions>
        <action name="start" timeout="5"/>
        <action name="stop" timeout="5"/>
	
	<!-- No-ops.  Groups are abstract resource types. 
        <action name="status" timeout="5" interval="1h"/>
        <action name="monitor" timeout="5" interval="1h"/>
 -->

        <action name="reconfig" timeout="5"/>
        <action name="recover" timeout="5"/>
        <action name="reload" timeout="5"/>
        <action name="meta-data" timeout="5"/>
        <action name="validate-all" timeout="5"/>
    </actions>
    
    <special tag="rgmanager">
        <attributes maxinstances="1"/>
        <child type="lvm" start="1" stop="9"/>
        <child type="fs" start="2" stop="8"/>
        <child type="clusterfs" start="3" stop="7"/>
        <child type="netfs" start="4" stop="6"/>
	<child type="nfsexport" start="5" stop="5"/>

	<child type="nfsclient" start="6" stop="4"/>

        <child type="ip" start="7" stop="2"/>
        <child type="smb" start="8" stop="3"/>
        <child type="script" start="9" stop="1"/>
    </special>
</resource-agent>
EOT
}


#
# A Resource group is abstract, but the OCF RA API doesn't allow for abstract
# resources, so here it is.
#
case $1 in
	start)
		#
		# XXX If this is set, we kill lockd.  If there is no
		# child IP address, then clients will NOT get the reclaim
		# notification.
		#
		if [ $NFS_TRICKS -eq 0 ]; then
			if [ "$OCF_RESKEY_nfslock" = "yes" ] || \
	   		   [ "$OCF_RESKEY_nfslock" = "1" ]; then
				pkill -KILL -x lockd
			fi
		fi
		exit 0
		;;
	stop)
		exit 0
		;;
	recover|restart)
		exit 0
		;;
	status|monitor)
		exit 0
		;;
	reload)
		exit 0
		;;
	meta-data)
		meta_data
		exit 0
		;;
	validate-all)
		exit 0
		;;
	reconfig)
		exit 0
		;;
	*)
		exit 0
		;;
esac
