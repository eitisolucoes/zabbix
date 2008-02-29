/* 
** ZABBIX
** Copyright (C) 2000-2005 SIA Zabbix
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**/

#include "common.h"

#include "zlog.h"
#include "db.h"
#include "sysinfo.h"
#include "daemon.h"
#include "zbxserver.h"

#include "poller.h"

#include "checks_agent.h"
#include "checks_aggregate.h"
#include "checks_external.h"
#include "checks_internal.h"
#include "checks_simple.h"
#include "checks_snmp.h"
#include "checks_db.h"

AGENT_RESULT    result;

static zbx_process_t	zbx_process;
int			poller_type;
int			poller_num;


int	get_value(DB_ITEM *item, AGENT_RESULT *result)
{
	int res=FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In get_value(key:%s)",
		item->key);

	alarm(CONFIG_TIMEOUT);

	if(item->type == ITEM_TYPE_ZABBIX)
	{
		res=get_value_agent(item, result);
	}
	else if( (item->type == ITEM_TYPE_SNMPv1) || (item->type == ITEM_TYPE_SNMPv2c) || (item->type == ITEM_TYPE_SNMPv3))
	{
#ifdef HAVE_SNMP
		res=get_value_snmp(item, result);
#else
		zabbix_log(LOG_LEVEL_WARNING, "Support of SNMP parameters was not compiled in");
		zabbix_syslog("Support of SNMP parameters was not compiled in. Cannot process [%s:%s]",
			item->host_name,
			item->key);
		res=NOTSUPPORTED;
#endif
	}
	else if(item->type == ITEM_TYPE_SIMPLE)
	{
		res=get_value_simple(item, result);
	}
	else if(item->type == ITEM_TYPE_INTERNAL)
	{
		res=get_value_internal(item, result);
	}
	else if(item->type == ITEM_TYPE_DB_MONITOR)
	{
		res=get_value_db(item, result);
	}
	else if(item->type == ITEM_TYPE_AGGREGATE)
	{
		res=get_value_aggregate(item, result);
	}
	else if(item->type == ITEM_TYPE_EXTERNAL)
	{
		res=get_value_external(item, result);
	}
	else
	{
		zabbix_log(LOG_LEVEL_WARNING, "Not supported item type:%d",
			item->type);
		zabbix_syslog("Not supported item type:%d",
			item->type);
		res=NOTSUPPORTED;
	}
	alarm(0);

	zabbix_log(LOG_LEVEL_DEBUG, "End get_value()");
	return res;
}

static int get_minnextcheck(int now)
{
	DB_RESULT	result;
	DB_ROW		row;

	int		res;

/* Host status	0 == MONITORED
		1 == NOT MONITORED
		2 == UNREACHABLE */
	if(poller_type == ZBX_POLLER_TYPE_UNREACHABLE)
	{
		result = DBselect("select count(*),min(nextcheck) as nextcheck from items i,hosts h"
				" where " ZBX_SQL_MOD(h.hostid,%d) "=%d and i.nextcheck<=%d and i.status in (%d)"
				" and i.type not in (%d,%d,%d) and h.status=%d and h.disable_until<=%d"
				" and h.errors_from!=0 and h.hostid=i.hostid and h.proxyid=0"
				" and i.key_ not in ('%s','%s','%s','%s')" DB_NODE " order by nextcheck",
			CONFIG_UNREACHABLE_POLLER_FORKS,
			poller_num-1,
			now,
			ITEM_STATUS_ACTIVE,
			ITEM_TYPE_TRAPPER, ITEM_TYPE_ZABBIX_ACTIVE, ITEM_TYPE_HTTPTEST,
			HOST_STATUS_MONITORED,
			now,
			SERVER_STATUS_KEY, SERVER_ICMPPING_KEY, SERVER_ICMPPINGSEC_KEY,SERVER_ZABBIXLOG_KEY,
			DBnode_local("h.hostid"));
	}
	else
	{
		if(CONFIG_REFRESH_UNSUPPORTED != 0)
		{
			result = DBselect("select count(*),min(nextcheck) from items i,hosts h"
					" where h.status=%d and h.disable_until<%d and h.errors_from=0"
					" and h.hostid=i.hostid and h.proxyid=0 and i.status in (%d,%d) and i.type not in (%d,%d,%d)"
					" and " ZBX_SQL_MOD(i.itemid,%d) "=%d and i.key_ not in ('%s','%s','%s','%s')" DB_NODE,
				HOST_STATUS_MONITORED,
				now,
				ITEM_STATUS_ACTIVE, ITEM_STATUS_NOTSUPPORTED,
				ITEM_TYPE_TRAPPER, ITEM_TYPE_ZABBIX_ACTIVE, ITEM_TYPE_HTTPTEST,
				CONFIG_POLLER_FORKS,
				poller_num-1,
				SERVER_STATUS_KEY, SERVER_ICMPPING_KEY, SERVER_ICMPPINGSEC_KEY,SERVER_ZABBIXLOG_KEY,
				DBnode_local("h.hostid"));
		}
		else
		{
			result = DBselect("select count(*),min(nextcheck) from items i,hosts h"
					" where h.status=%d and h.disable_until<%d and h.errors_from=0"
					" and h.hostid=i.hostid and h.proxyid=0 and i.status in (%d) and i.type not in (%d,%d,%d)"
					" and " ZBX_SQL_MOD(i.itemid,%d) "=%d and i.key_ not in ('%s','%s','%s','%s')" DB_NODE,
				HOST_STATUS_MONITORED,
				now,
				ITEM_STATUS_ACTIVE,
				ITEM_TYPE_TRAPPER, ITEM_TYPE_ZABBIX_ACTIVE, ITEM_TYPE_HTTPTEST,
				CONFIG_POLLER_FORKS,
				poller_num-1,
				SERVER_STATUS_KEY, SERVER_ICMPPING_KEY, SERVER_ICMPPINGSEC_KEY,SERVER_ZABBIXLOG_KEY,
				DBnode_local("h.hostid"));
		}
	}

	row=DBfetch(result);

	if(!row || DBis_null(row[0])==SUCCEED || DBis_null(row[1])==SUCCEED)
	{
		zabbix_log(LOG_LEVEL_DEBUG, "No items to update for minnextcheck.");
		res = FAIL; 
	}
	else
	{
		if( atoi(row[0]) == 0)
		{
			res = FAIL;
		}
		else
		{
			res = atoi(row[1]);
		}
	}
	DBfree_result(result);

	return	res;
}

/* Update special host's item - "status" */
static void update_key_status(zbx_uint64_t hostid, int host_status, time_t now)
{
/*	char		value_str[MAX_STRING_LEN];*/
	AGENT_RESULT	agent;

	DB_ITEM		item;
	DB_RESULT	result;
	DB_ROW		row;

	int		update;

	zabbix_log(LOG_LEVEL_DEBUG, "In update_key_status(" ZBX_FS_UI64 ",%d)",
		hostid,
		host_status);

	result = DBselect("select %s where h.hostid=i.hostid and h.proxyid=0 and h.hostid=" ZBX_FS_UI64 " and i.key_='%s'",
		ZBX_SQL_ITEM_SELECT,
		hostid,
		SERVER_STATUS_KEY);

	while (NULL != (row = DBfetch(result))) {
		DBget_item_from_db(&item,row);

/* Do not process new value for status, if previous status is the same */
		update = (item.lastvalue_null==1);
		update = update || ((item.value_type == ITEM_VALUE_TYPE_FLOAT) &&(cmp_double(item.lastvalue_dbl, (double)host_status) == 1));
		update = update || ((item.value_type == ITEM_VALUE_TYPE_UINT64) &&(item.lastvalue_uint64 != host_status));

		if (update) {
			init_result(&agent);
			SET_UI64_RESULT(&agent, host_status);

			switch (zbx_process) {
			case ZBX_PROCESS_SERVER:
				process_new_value(&item, &agent, now);
				update_triggers(item.itemid);
				break;
			case ZBX_PROCESS_PROXY:
				proxy_process_new_value(&item, &agent, now);
				break;
			}

			free_result(&agent);
		}
	}

	DBfree_result(result);
}

static void enable_host(DB_ITEM *item, time_t now, char *error)
{
	assert(item);

	zabbix_log(LOG_LEVEL_WARNING, "Enabling host [%s]",
			item->host_name);
	zabbix_syslog("Enabling host [%s]",
			item->host_name);

	switch (zbx_process) {
	case ZBX_PROCESS_SERVER:
		DBupdate_host_availability(item->hostid, HOST_AVAILABLE_TRUE, now, error);
		update_key_status(item->hostid, HOST_STATUS_MONITORED, now); /* 0 */
		break;
	case ZBX_PROCESS_PROXY:
		DBproxy_update_host_availability(item->hostid, HOST_AVAILABLE_TRUE, now);
		break;
	}

	item->host_available = HOST_AVAILABLE_TRUE;
}

static void disable_host(DB_ITEM *item, time_t now, char *error)
{
	assert(item);

	zabbix_log(LOG_LEVEL_WARNING, "Host [%s] will be checked after %d seconds",
			item->host_name,
			CONFIG_UNAVAILABLE_DELAY);
	zabbix_syslog("Host [%s] will be checked after %d seconds",
			item->host_name,
			CONFIG_UNAVAILABLE_DELAY);

	switch (zbx_process) {
	case ZBX_PROCESS_SERVER:
		DBupdate_host_availability(item->hostid, HOST_AVAILABLE_FALSE, now, error);
		update_key_status(item->hostid, HOST_AVAILABLE_FALSE, now); /* 2 */
		break;
	case ZBX_PROCESS_PROXY:
		DBproxy_update_host_availability(item->hostid, HOST_AVAILABLE_FALSE, now);
		break;
	}

	item->host_available = HOST_AVAILABLE_FALSE;

	DBexecute("update hosts set disable_until=%d where hostid=" ZBX_FS_UI64,
		now + CONFIG_UNAVAILABLE_DELAY,
		item->hostid);
}

/******************************************************************************
 *                                                                            *
 * Function: get_values                                                       *
 *                                                                            *
 * Purpose: retrieve values of metrics from monitored hosts                   *
 *                                                                            *
 * Parameters:                                                                *
 *                                                                            *
 * Return value:                                                              *
 *                                                                            *
 * Author: Alexei Vladishev                                                   *
 *                                                                            *
 * Comments: always SUCCEED                                                   *
 *                                                                            *
 ******************************************************************************/
int get_values(void)
{
	DB_RESULT	result;
	DB_RESULT	result2;
	DB_ROW	row;
	DB_ROW	row2;

	time_t		now;
	int		delay;
	int		res;
	DB_ITEM		item;
	AGENT_RESULT	agent;
	int		stop = 0, items = 0;

	char		*unreachable_hosts = NULL;
	char		tmp[MAX_STRING_LEN];

	zabbix_log( LOG_LEVEL_DEBUG, "In get_values()");

	now = time(NULL);

	zbx_snprintf(tmp,sizeof(tmp)-1,ZBX_FS_UI64,0);
	unreachable_hosts=zbx_strdcat(unreachable_hosts,tmp);

	/* Poller for unreachable hosts */
	if(poller_type == ZBX_POLLER_TYPE_UNREACHABLE)
	{
		result = DBselect("select h.hostid,min(i.itemid) from hosts h,items i"
				" where " ZBX_SQL_MOD(h.hostid,%d) "=%d and i.nextcheck<=%d and i.status in (%d)"
				" and i.type not in (%d,%d,%d) and h.status=%d and h.disable_until<=%d"
				" and h.errors_from!=0 and h.hostid=i.hostid and h.proxyid=0"
				" and i.key_ not in ('%s','%s','%s','%s')" DB_NODE " group by h.hostid",
			CONFIG_UNREACHABLE_POLLER_FORKS,
			poller_num-1,
			now,
			ITEM_STATUS_ACTIVE,
			ITEM_TYPE_TRAPPER, ITEM_TYPE_ZABBIX_ACTIVE, ITEM_TYPE_HTTPTEST,
			HOST_STATUS_MONITORED,
			now,
			SERVER_STATUS_KEY, SERVER_ICMPPING_KEY, SERVER_ICMPPINGSEC_KEY,SERVER_ZABBIXLOG_KEY,
			DBnode_local("h.hostid"));
	}
	else
	{
		if(CONFIG_REFRESH_UNSUPPORTED != 0)
		{
			result = DBselect("select %s where i.nextcheck<=%d and i.status in (%d,%d)"
					" and i.type not in (%d,%d,%d) and h.status=%d and h.disable_until<=%d"
					" and h.errors_from=0 and h.hostid=i.hostid and h.proxyid=0"
					" and " ZBX_SQL_MOD(i.itemid,%d) "=%d and i.key_ not in ('%s','%s','%s','%s')"
					DB_NODE " order by i.nextcheck",
				ZBX_SQL_ITEM_SELECT,
				now,
				ITEM_STATUS_ACTIVE, ITEM_STATUS_NOTSUPPORTED,
				ITEM_TYPE_TRAPPER, ITEM_TYPE_ZABBIX_ACTIVE, ITEM_TYPE_HTTPTEST,
				HOST_STATUS_MONITORED,
				now,
				CONFIG_POLLER_FORKS,
				poller_num-1,
				SERVER_STATUS_KEY, SERVER_ICMPPING_KEY, SERVER_ICMPPINGSEC_KEY,SERVER_ZABBIXLOG_KEY,
				DBnode_local("h.hostid"));
		}
		else
		{
			result = DBselect("select %s where i.nextcheck<=%d and i.status in (%d)"
					" and i.type not in (%d,%d,%d) and h.status=%d and h.disable_until<=%d"
					" and h.errors_from=0 and h.hostid=i.hostid and h.proxyid=0"
					" and " ZBX_SQL_MOD(i.itemid,%d) "=%d and i.key_ not in ('%s','%s','%s','%s')"
					DB_NODE " order by i.nextcheck",
				ZBX_SQL_ITEM_SELECT,
				now,
				ITEM_STATUS_ACTIVE,
				ITEM_TYPE_TRAPPER, ITEM_TYPE_ZABBIX_ACTIVE, ITEM_TYPE_HTTPTEST,
				HOST_STATUS_MONITORED,
				now,
				CONFIG_POLLER_FORKS,
				poller_num-1,
				SERVER_STATUS_KEY, SERVER_ICMPPING_KEY, SERVER_ICMPPINGSEC_KEY,SERVER_ZABBIXLOG_KEY,
				DBnode_local("h.hostid"));
		}
	}

	/* Do not stop when select is made by poller for unreachable hosts */
	while((row=DBfetch(result))&&(stop==0 || poller_type == ZBX_POLLER_TYPE_UNREACHABLE))
	{
		/* This code is just to avoid compilation warining about use of uninitialized result2 */
		result2 = result;
		/* */

		/* Poller for unreachable hosts */
		if(poller_type == ZBX_POLLER_TYPE_UNREACHABLE)
		{
			result2 = DBselect("select %s where h.hostid=i.hostid and h.proxyid=0 and i.itemid=%s" DB_NODE,
				ZBX_SQL_ITEM_SELECT,
				row[1],
				DBnode_local("h.hostid"));

			row2 = DBfetch(result2);

			if(!row2)
			{
				DBfree_result(result2);
				continue;
			}
			DBget_item_from_db(&item,row2);
		}
		else
		{
			DBget_item_from_db(&item,row);
			/* Skip unreachable hosts but do not break the loop. */
			if(uint64_in_list(unreachable_hosts,item.hostid) == SUCCEED)
			{
				zabbix_log( LOG_LEVEL_DEBUG, "Host " ZBX_FS_UI64 " is unreachable. Skipping [%s]",
					item.hostid,item.key);
				continue;
			}
		}

		init_result(&agent);

		res = get_value(&item, &agent);

		now = time(NULL);

		DBbegin();
		
		if(res == SUCCEED )
		{
			switch (zbx_process) {
			case ZBX_PROCESS_SERVER:
				process_new_value(&item, &agent, now);
				break;
			case ZBX_PROCESS_PROXY:
				proxy_process_new_value(&item, &agent, now);
				break;
			}

			if (HOST_AVAILABLE_TRUE != item.host_available) {
				enable_host(&item, now, agent.msg);
				stop = 1;
			}
			if (item.host_errors_from != 0) {
				DBexecute("update hosts set errors_from=0 where hostid=" ZBX_FS_UI64,
						item.hostid);
				stop = 1;
			}

			switch (zbx_process) {
			case ZBX_PROCESS_SERVER:
				update_triggers(item.itemid);
				break;
			default:
				/* nothing */;
			}

		}
		else if(res == NOTSUPPORTED || res == AGENT_ERROR)
		{
			if(item.status == ITEM_STATUS_NOTSUPPORTED)
			{
				/* It is not correct */
/*				snprintf(sql,sizeof(sql)-1,"update items set nextcheck=%d, lastclock=%d where itemid=%d",calculate_item_nextcheck(item.itemid, CONFIG_REFRESH_UNSUPPORTED,now), now, item.itemid);*/
				DBexecute("update items set nextcheck=%d, lastclock=%d where itemid=" ZBX_FS_UI64,
					CONFIG_REFRESH_UNSUPPORTED+now,
					now,
					item.itemid);
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "Parameter [%s] is not supported by agent on host [%s] Old status [%d]",
						item.key,
						item.host_name,
						item.status);
				zabbix_syslog("Parameter [%s] is not supported by agent on host [%s]",
						item.key,
						item.host_name);

				switch (zbx_process) {
				case ZBX_PROCESS_SERVER:
					DBupdate_item_status_to_notsupported(item.itemid, agent.msg);
					break;
				case ZBX_PROCESS_PROXY:
					DBproxy_update_item_status_to_notsupported(item.itemid);
					break;
				}

	/*			if(HOST_STATUS_UNREACHABLE == item.host_status)*/
				if (HOST_AVAILABLE_TRUE != item.host_available) {
					enable_host(&item, now, agent.msg);
					stop = 1;
				}
			}
		}
		else if(res == NETWORK_ERROR)
		{
			/* First error */
			if(item.host_errors_from==0)
			{
				zabbix_log( LOG_LEVEL_WARNING, "Host [%s]: first network error, wait for %d seconds",
					item.host_name,
					CONFIG_UNREACHABLE_DELAY);
				zabbix_syslog("Host [%s]: first network error, wait for %d seconds",
					item.host_name,
					CONFIG_UNREACHABLE_DELAY);

				item.host_errors_from=now;
				DBexecute("update hosts set errors_from=%d,disable_until=%d where hostid=" ZBX_FS_UI64,
					now,
					now+CONFIG_UNREACHABLE_DELAY,
					item.hostid);

				delay = MIN(4*item.delay, 300);
				zabbix_log( LOG_LEVEL_WARNING, "Parameter [%s] will be checked after %d seconds on host [%s]",
					item.key,
					delay,
					item.host_name);
				DBexecute("update items set nextcheck=%d where itemid=" ZBX_FS_UI64,
					now + delay,
					item.itemid);
			}
			else
			{
				if (now - item.host_errors_from > CONFIG_UNREACHABLE_PERIOD) {
					disable_host(&item, now, agent.msg);
				}
				/* Still unavailable, but won't change status to UNAVAILABLE yet */
				else
				{
					zabbix_log( LOG_LEVEL_WARNING, "Host [%s]: another network error, wait for %d seconds",
						item.host_name,
						CONFIG_UNREACHABLE_DELAY);
					zabbix_syslog("Host [%s]: another network error, wait for %d seconds",
						item.host_name,
						CONFIG_UNREACHABLE_DELAY);

					DBexecute("update hosts set disable_until=%d where hostid=" ZBX_FS_UI64,
						now+CONFIG_UNREACHABLE_DELAY,
						item.hostid);
				}
			}

			zbx_snprintf(tmp,sizeof(tmp)-1,"," ZBX_FS_UI64,item.hostid);
			unreachable_hosts=zbx_strdcat(unreachable_hosts,tmp);

/*			stop=1;*/
		}
		else
		{
			zabbix_log( LOG_LEVEL_CRIT, "Unknown response code returned.");
			assert(0==1);
		}
		/* Poller for unreachable hosts */
		if(poller_type == ZBX_POLLER_TYPE_UNREACHABLE)
		{
			/* We cannot freeit earlier because items has references to the structure */
			DBfree_result(result2);
		}
		free_result(&agent);
		DBcommit();

		items++;
	}

	zbx_free(unreachable_hosts);

	DBfree_result(result);
	zabbix_log( LOG_LEVEL_DEBUG, "End get_values()");
	return items;
}

void main_poller_loop(zbx_process_t p, int type, int num)
{
	struct	sigaction phan;
	int	now;
	int	nextcheck, sleeptime;
	int	items;
	double	sec;

	zabbix_log( LOG_LEVEL_DEBUG, "In main_poller_loop(type:%d,num:%d)",
			type,
			num);

	phan.sa_handler = child_signal_handler;
	sigemptyset(&phan.sa_mask);
	phan.sa_flags = 0;
	sigaction(SIGALRM, &phan, NULL);

	zbx_process	= p;
	poller_type	= type;
	poller_num	= num;

	DBconnect(ZBX_DB_CONNECT_NORMAL);

	for (;;) {
		zbx_setproctitle("poller [getting values]");

		now = time(NULL);

		sec = zbx_time();
		items = get_values();
		sec = zbx_time() - sec;

		nextcheck = get_minnextcheck(now);

		zabbix_log(LOG_LEVEL_DEBUG, "Poller spent " ZBX_FS_DBL " seconds while updating %3d values. Nextcheck: %d Time: %d",
				sec,
				items,
				nextcheck,
				(int)time(NULL));

		if( FAIL == nextcheck)
		{
			sleeptime=POLLER_DELAY;
		}
		else
		{
			sleeptime=nextcheck-time(NULL);
			if(sleeptime<0)
			{
				sleeptime=0;
			}
		}
		if(sleeptime>0)
		{
			if(sleeptime > POLLER_DELAY)
			{
				sleeptime = POLLER_DELAY;
			}
			zabbix_log( LOG_LEVEL_DEBUG, "Sleeping for %d seconds",
					sleeptime );

			zbx_setproctitle("poller [sleeping for %d seconds]", 
					sleeptime);

			sleep( sleeptime );
		}
		else
		{
			zabbix_log( LOG_LEVEL_DEBUG, "No sleeping" );
		}
	}
}
