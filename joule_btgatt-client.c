/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>

#include "oc_list.h"
#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "lib/uuid.h"

#include "src/shared/mainloop.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-client.h"

#include "joule_btgatt-client.h"
#define ATT_CID 4

#define printf(...) \
	printf(__VA_ARGS__);

#define COLOR_OFF	"\x1B[0m"
#define COLOR_RED	"\x1B[0;91m"
#define COLOR_GREEN	"\x1B[0;92m"
#define COLOR_YELLOW	"\x1B[0;93m"
#define COLOR_BLUE	"\x1B[0;94m"
#define COLOR_MAGENTA	"\x1B[0;95m"
#define COLOR_BOLDGRAY	"\x1B[1;30m"
#define COLOR_BOLDWHITE	"\x1B[1;37m"

OC_LIST(ready_cb_list);
OC_LIST(read_cb_list);
OC_LIST(write_cb_list);
OC_LIST(notify_cb_list);

struct client {
	int fd;
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_client *gatt;
	OC_LIST_STRUCT(service_db);
	unsigned int reliable_session_id;
};

static bdaddr_t src_addr;
static pthread_t event_thread;
static pthread_mutex_t ready_cb_mutex,read_cb_mutex,write_cb_mutex,notify_cb_mutex;
/*mutex */
static int mutex_init(pthread_mutex_t * mutex_obj)
{
	if (pthread_mutex_init(mutex_obj, NULL) != 0)
	{
		printf("initializing mutex failed");
		return -1;
	}
	return 0;
}

static void mutex_lock(pthread_mutex_t * mutex_obj)
{
	pthread_mutex_lock(mutex_obj);
}

static void mutex_unlock(pthread_mutex_t * mutex_obj)
{
	pthread_mutex_unlock(mutex_obj);
}

static void mutex_destroy(pthread_mutex_t * mutex_obj)
{
	pthread_mutex_destroy(mutex_obj);
}

static void print_uuid(const bt_uuid_t *uuid)
{
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t uuid128;

	bt_uuid_to_uuid128(uuid, &uuid128);
	bt_uuid_to_string(&uuid128, uuid_str, sizeof(uuid_str));

	printf("%s\n", uuid_str);
}

static int check_cb_validity(int callback_id,void *head)
{
	cb_valid_list *obj = head;
	while(obj)
	{
		if(obj->cb_valid_id == callback_id)
		{
			break;
		}
		obj = obj->next;
	}

	if(obj)
		return VALID;

	return INVALID;
}

static const char *ecode_to_string(uint8_t ecode)
{
	switch (ecode) {
	case BT_ATT_ERROR_INVALID_HANDLE:
		return "Invalid Handle";
	case BT_ATT_ERROR_READ_NOT_PERMITTED:
		return "Read Not Permitted";
	case BT_ATT_ERROR_WRITE_NOT_PERMITTED:
		return "Write Not Permitted";
	case BT_ATT_ERROR_INVALID_PDU:
		return "Invalid PDU";
	case BT_ATT_ERROR_AUTHENTICATION:
		return "Authentication Required";
	case BT_ATT_ERROR_REQUEST_NOT_SUPPORTED:
		return "Request Not Supported";
	case BT_ATT_ERROR_INVALID_OFFSET:
		return "Invalid Offset";
	case BT_ATT_ERROR_AUTHORIZATION:
		return "Authorization Required";
	case BT_ATT_ERROR_PREPARE_QUEUE_FULL:
		return "Prepare Write Queue Full";
	case BT_ATT_ERROR_ATTRIBUTE_NOT_FOUND:
		return "Attribute Not Found";
	case BT_ATT_ERROR_ATTRIBUTE_NOT_LONG:
		return "Attribute Not Long";
	case BT_ATT_ERROR_INSUFFICIENT_ENCRYPTION_KEY_SIZE:
		return "Insuficient Encryption Key Size";
	case BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN:
		return "Invalid Attribute value len";
	case BT_ATT_ERROR_UNLIKELY:
		return "Unlikely Error";
	case BT_ATT_ERROR_INSUFFICIENT_ENCRYPTION:
		return "Insufficient Encryption";
	case BT_ATT_ERROR_UNSUPPORTED_GROUP_TYPE:
		return "Group type Not Supported";
	case BT_ATT_ERROR_INSUFFICIENT_RESOURCES:
		return "Insufficient Resources";
	case BT_ERROR_CCC_IMPROPERLY_CONFIGURED:
		return "CCC Improperly Configured";
	case BT_ERROR_ALREADY_IN_PROGRESS:
		return "Procedure Already in Progress";
	case BT_ERROR_OUT_OF_RANGE:
		return "Out of Range";
	default:
		return "Unknown error type";
	}
}

static void ready_cb(bool success, uint8_t att_ecode, void *user_data)
{
	mutex_lock(&ready_cb_mutex);
	ready_cb_data *data = user_data;
	if( check_cb_validity(data->cb_valid_id,oc_list_head(ready_cb_list)) == INVALID)
	{
		free(data);
		goto unlock;
	}

	if (!success)
	{
		printf("GATT discovery procedures failed - error code: 0x%02x\n",
								att_ecode);
		data->flag = REQUEST_COMPLETED;
		goto unlock;
	}
	data->flag = REQUEST_SUCCESS; // if successfully completed

unlock:
	mutex_unlock(&ready_cb_mutex);
}

static void get_uuid_string(const bt_uuid_t *uuid, char *uuid_str)
{
	bt_uuid_t uuid128;

	bt_uuid_to_uuid128(uuid, &uuid128);
	bt_uuid_to_string(&uuid128, uuid_str, MAX_LEN_UUID_STR);

	printf("%s\n", uuid_str);
}

static void populate_chrc_database(struct gatt_db_attribute *attr, void *user_data)
{
	struct client *cli = user_data;
	uint16_t handle, value_handle;
	uint8_t properties;
	uint16_t ext_prop;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, &handle,
								&value_handle,
								&properties,
								&ext_prop,
								&uuid))
		return;

	printf("\t  " COLOR_YELLOW "charac" COLOR_OFF
				" - start: 0x%04x, value: 0x%04x, "
				"props: 0x%02x, ext_props: 0x%04x, uuid: ",
				handle, value_handle, properties, ext_prop);
	print_uuid(&uuid);
	chrc_db *chrc = (chrc_db *) malloc0(sizeof(chrc_db));
	if(chrc == NULL)
	{
		printf("cannot allocate memory for characteristic database population\n");
		return;
	}
	chrc->handle = value_handle;
	get_uuid_string(&uuid,chrc->uuid_str);
	oc_list_add(cli->service_db,chrc);
	//gatt_db_service_foreach_desc(attr, print_desc, NULL);
}

static void parse_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct client *cli = user_data;
	uint16_t start, end;
	bool primary;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_service_data(attr, &start, &end, &primary,
									&uuid))
		return;

	printf(COLOR_RED "service" COLOR_OFF " - start: 0x%04x, "
				"end: 0x%04x, type: %s, uuid: ",
				start, end, primary ? "primary" : "secondary");
	print_uuid(&uuid);

	//gatt_db_service_foreach_incl(attr, print_incl, cli);
	gatt_db_service_foreach_char(attr, populate_chrc_database, cli);

	printf("\n");
}

static void populate_services_cli_db(struct client *cli)
{
	OC_LIST_STRUCT_INIT(cli, service_db);
	gatt_db_foreach_service(cli->db, NULL, parse_service, cli);
}

static struct client *client_create(int fd, uint16_t mtu,bt_att_disconnect_func_t callback, void *user_data)
{
	struct client *cli;

	cli = new0(struct client, 1);
	if (!cli) {
		printf( "Failed to allocate memory for client\n");
		return NULL;
	}

	cli->att = bt_att_new(fd, false);
	if (!cli->att) {
		printf( "Failed to initialze ATT transport layer\n");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	if (!bt_att_set_close_on_unref(cli->att, true)) {
		printf( "Failed to set up ATT transport layer\n");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	if (!bt_att_register_disconnect(cli->att, callback, user_data,
								NULL)) {
		printf( "Failed to set ATT disconnect handler\n");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	cli->fd = fd;
	cli->db = gatt_db_new();
	if (!cli->db) {
		printf( "Failed to create GATT database\n");
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	cli->gatt = bt_gatt_client_new(cli->db, cli->att, mtu);
	if (!cli->gatt) {
		printf( "Failed to create GATT client\n");
		gatt_db_unref(cli->db);
		bt_att_unref(cli->att);
		free(cli);
		return NULL;
	}

	/* create cb user_data */
	ready_cb_data *cb_data = malloc0(sizeof(ready_cb_data));
	if(!cb_data)
		goto cleanup;

	cb_data->cb_valid_id = rand();
	cb_data->flag = REQUEST_GENERATED;

	/* create list node data */
	cb_valid_list *list_data = malloc0(sizeof(cb_valid_list));
	if(!list_data)
	{
		free(cb_data);
		goto cleanup;
	}
	list_data->cb_valid_id = cb_data->cb_valid_id;

	oc_list_add(ready_cb_list,list_data);

	bt_gatt_client_ready_register(cli->gatt, ready_cb, cb_data, NULL);

	//bt_gatt_client_set_service_changed(cli->gatt, service_changed_cb, cli,NULL);
	/* wait for it to be ready*/
	int counter =0;
	while(cb_data->flag == REQUEST_GENERATED)
	{
		if(counter < 5)
		{
			mutex_unlock(&ready_cb_mutex);
			sleep(1);
			counter++;
			mutex_lock(&ready_cb_mutex);
			continue;
		}
		goto unlock;
	}
	/* successful */
	oc_list_remove(ready_cb_list,list_data);
	mutex_unlock(&ready_cb_mutex);
	free(list_data);
	free(cb_data);
	populate_services_cli_db(cli);
	/* bt_gatt_client already holds a reference */
	gatt_db_unref(cli->db);

	return cli;

unlock:
	oc_list_remove(ready_cb_list,list_data);
	mutex_unlock(&ready_cb_mutex);
	free(list_data);
	printf( "Failed to discover services\n");
cleanup:
	gatt_db_unref(cli->db);
	bt_att_unref(cli->att);
	free(cli);
	return NULL;
}

static void client_destroy(struct client *cli)
{
	bt_gatt_client_unref(cli->gatt);
	bt_att_unref(cli->att);
	free(cli);
}

static uint16_t get_handle_for_uuid_str(struct client *cli,char *uuid_str)
{
	chrc_db *chrc = oc_list_head(cli->service_db);
	while(chrc != NULL)
	{
		if(!strcmp(chrc->uuid_str,uuid_str))
		{
			return chrc->handle;
		}
		chrc = chrc->next;
	}

	return 0;
}

static void read_cb(bool success, uint8_t att_ecode, const uint8_t *value,
					uint16_t length, void *user_data)
{
	mutex_lock(&read_cb_mutex);
	read_cb_data *data = user_data;

	if( check_cb_validity(data->cb_valid_id,oc_list_head(read_cb_list)) == INVALID)
	{
		free(data);
		goto unlock;
	}

	if (!success) {
		printf("\nRead request failed: %s (0x%02x)\n",
				ecode_to_string(att_ecode), att_ecode);
		data->flag=REQUEST_COMPLETED;
		goto unlock;
	}
	memcpy(data->value, value,length);
	data->length = length;
	data->flag=REQUEST_SUCCESS;
unlock:
	mutex_unlock(&read_cb_mutex);
}

int joule_read(struct client *cli, char *uuid_str,uint8_t *value,int *len)
{
	int ret;
	if (!bt_gatt_client_is_ready(cli->gatt))
	{
		printf("GATT client not initialized\n");
		return -1;
	}

	uint16_t handle = get_handle_for_uuid_str(cli,uuid_str);
	if(handle == 0)
	{
		printf("Failed to find handle\n");
		return -1;
	}
	/* create cb user data */
	read_cb_data *cb_data = malloc0(sizeof(read_cb_data));
	if(!cb_data)
	{
		printf("Failed to allocate memory\n");
		return -1;
	}

	cb_data->value = value;
	cb_data->cb_valid_id = rand();
	cb_data->flag = REQUEST_GENERATED;

	/* create list node for cb*/
	cb_valid_list *list_data = malloc0(sizeof(cb_valid_list));
	if(!list_data)
	{
		free(cb_data);
		printf("Failed to allocate memory for list\n");
		return -1;
	}
	list_data->cb_valid_id = cb_data->cb_valid_id;

	oc_list_add(read_cb_list,list_data);

	/* read value */
	if (!bt_gatt_client_read_value(cli->gatt, handle, read_cb,
			cb_data, NULL))
	{
		free(cb_data);
		mutex_lock(&read_cb_mutex);
		ret = -1;
		goto exit_func;
	}
	/* wait for callback */
	int counter =0;
	while(cb_data->flag == REQUEST_GENERATED )
	{
		if(counter < 5)
		{
			mutex_unlock(&read_cb_mutex);
			sleep(1);
			counter++;
			mutex_lock(&read_cb_mutex);
			continue;
		}
		break;
	}

	if(cb_data->flag != REQUEST_GENERATED)
	{
		ret = cb_data->flag == REQUEST_COMPLETED ? -1 : 0;
		if(! ret) //Successful
		{
			memcpy(value,cb_data->value,cb_data->length);
			*len = cb_data->length;
		}
		free(cb_data);
	}
	else
		ret = -1;

exit_func:
	oc_list_remove(read_cb_list,list_data);
	mutex_unlock(&read_cb_mutex);
	free(list_data);
	return ret;
}

static void write_cb(bool success, uint8_t att_ecode, void *user_data)
{
	mutex_lock(&write_cb_mutex);
	read_cb_data *data = user_data;

	if( check_cb_validity(data->cb_valid_id,oc_list_head(write_cb_list)) == INVALID)
	{
		free(data);
		goto unlock;
	}

	if (success)
	{
		data->flag = REQUEST_SUCCESS;
	}
	else
	{
		printf("\nWrite failed: %s (0x%02x)\n",
				ecode_to_string(att_ecode), att_ecode);
		data->flag = REQUEST_COMPLETED;
	}
unlock:
	mutex_unlock(&write_cb_mutex);
}

int joule_write_value(struct client *cli, char *uuid_str,uint8_t *value,uint16_t length,bool response)
{
	bool signed_write = false;
	int ret=0;
	if (!bt_gatt_client_is_ready(cli->gatt))
	{
		printf("GATT client not initialized\n");
		return -1;
	}

	uint16_t handle = get_handle_for_uuid_str(cli,uuid_str);
	if(handle == 0)
	{
		printf("Failed to find handle\n");
		return -1;
	}

	if (!response)
	{
		if (!bt_gatt_client_write_without_response(cli->gatt, handle,
						signed_write, value, length))
		{
			printf("Failed to initiate write without response "
								"procedure\n");
			return -1;
		}

		return 0;
	}

	/* requires a response */

	/* create cb user data */
	write_cb_data *cb_data = malloc0(sizeof(write_cb_data));
	if(!cb_data)
	{
		printf("Failed to allocate memory\n");
		return -1;
	}

	cb_data->cb_valid_id = rand();
	cb_data->flag = REQUEST_GENERATED;

	/* create list node for cb*/
	cb_valid_list *list_data = malloc0(sizeof(cb_valid_list));
	if(!list_data)
	{
		free(cb_data);
		printf("Failed to allocate memory for list\n");
		return -1;
	}
	list_data->cb_valid_id = cb_data->cb_valid_id;
	oc_list_add(write_cb_list,list_data);

	if (!bt_gatt_client_write_value(cli->gatt, handle, value, length,
								write_cb,
								NULL, NULL))
	{
		free(cb_data);
		mutex_lock(&write_cb_mutex);
		ret = -1;
		goto exit_func;
	}
	/* wait for callback */
	int counter =0;
	while(cb_data->flag == REQUEST_GENERATED )
	{
		if(counter < 5)
		{
			mutex_unlock(&write_cb_mutex);
			sleep(1);
			counter++;
			mutex_lock(&write_cb_mutex);
			continue;
		}
		break;
	}

	if(cb_data->flag != REQUEST_GENERATED)
	{
		ret = cb_data->flag == REQUEST_COMPLETED ? -1 : 0;
		free(cb_data);
	}
	else
		ret = -1;

exit_func:
	oc_list_remove(write_cb_list,list_data);
	mutex_unlock(&write_cb_mutex);
	free(list_data);
	return ret;
}

static void notify_cb(uint16_t value_handle, const uint8_t *value,
					uint16_t length, void *user_data)
{
	int i;

	printf("\n\tHandle Value Not/Ind: 0x%04x - ", value_handle);

	if (length == 0) {
		printf("(0 bytes)\n");
		return;
	}

	printf("(%u bytes): ", length);

	for (i = 0; i < length; i++)
		printf("%02x ", value[i]);

	printf("\n");
}

static void destory_notify_cb(void *user_data)
{
	printf("%s called\n",__func__);
	notify_cb_data *data = user_data;
	if(data->flag == REQUEST_SUCCESS)free(user_data);
}
static void register_notify_cb(uint16_t att_ecode, void *user_data)
{
	mutex_lock(&notify_cb_mutex);
	notify_cb_data *data = user_data;

	if( check_cb_validity(data->cb_valid_id,oc_list_head(notify_cb_list)) == INVALID)
	{
		free(data);
		goto unlock;
	}

	if (att_ecode) {
		printf("Failed to register notify handler "
					"- error code: 0x%02x\n", att_ecode);
		data->flag = REQUEST_COMPLETED;
	}else
		data->flag = REQUEST_SUCCESS;

unlock:
	mutex_unlock(&notify_cb_mutex);
}

int joule_register_notify(struct client *cli, char *uuid_str,notify_callback_t func,void *user_data)
{
	unsigned int id;
	int ret;

	if (!bt_gatt_client_is_ready(cli->gatt)) {
		printf("GATT client not initialized\n");
		return -1;
	}

	uint16_t handle = get_handle_for_uuid_str(cli,uuid_str);
	if(handle == 0)
	{
		printf("Failed to find handle\n");
		return -1;
	}

	/* create cb user data */
	notify_cb_data *cb_data = malloc0(sizeof(notify_cb_data));
	if(!cb_data)
	{
		printf("Failed to allocate memory\n");
		return -1;
	}

	cb_data->cb_valid_id = rand();
	cb_data->flag = REQUEST_GENERATED;
	cb_data->ptr = user_data;
	/* create list node for cb*/
	cb_valid_list *list_data = malloc0(sizeof(cb_valid_list));
	if(!list_data)
	{
		free(cb_data);
		printf("Failed to allocate memory for list\n");
		return -1;
	}
	list_data->cb_valid_id = cb_data->cb_valid_id;
	oc_list_add(notify_cb_list,list_data);

	id = bt_gatt_client_register_notify(cli->gatt, handle,
							register_notify_cb,
							notify_cb, cb_data, destory_notify_cb);
	if (!id)
	{
		free(cb_data);
		mutex_lock(&notify_cb_mutex);
		ret = -1;
		goto exit_func;
	}
	/* wait for callback */
	int counter =0;
	while(cb_data->flag == REQUEST_GENERATED )
	{
		if(counter < 5)
		{
			mutex_unlock(&notify_cb_mutex);
			sleep(1);
			counter++;
			mutex_lock(&notify_cb_mutex);
			continue;
		}
		break;
	}

	if(cb_data->flag != REQUEST_GENERATED)
	{
		ret = cb_data->flag == REQUEST_COMPLETED ? -1 : 0;
		free(cb_data);
	}
	else
		ret = -1;

exit_func:
	oc_list_remove(notify_cb_list,list_data);
	mutex_unlock(&notify_cb_mutex);
	free(list_data);
	return ret;

	printf("Registering notify handler with id: %u\n", id);
}

int joule_unregister_notify(struct client *cli, int id)
{
	if (!bt_gatt_client_is_ready(cli->gatt))
	{
		printf("GATT client not initialized\n");
		return -1;
	}

	if (!bt_gatt_client_unregister_notify(cli->gatt, id))
	{
		printf("Failed to unregister notify handler with id: %u\n", id);
		return -1;
	}

	return 0;
}

static int l2cap_le_att_connect(bdaddr_t *src, bdaddr_t *dst, uint8_t dst_type,
									int sec)
{
	int sock;
	struct sockaddr_l2 srcaddr, dstaddr;
	struct bt_security btsec;

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sock < 0) {
		perror("Failed to create L2CAP socket");
		return -1;
	}

	/* Set up source address */
	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = 0;
	bacpy(&srcaddr.l2_bdaddr, src);

	if (bind(sock, (struct sockaddr *)&srcaddr, sizeof(srcaddr)) < 0) {
		perror("Failed to bind L2CAP socket");
		close(sock);
		return -1;
	}

	/* Set the security level */
	memset(&btsec, 0, sizeof(btsec));
	btsec.level = sec;
	if (setsockopt(sock, SOL_BLUETOOTH, BT_SECURITY, &btsec,
							sizeof(btsec)) != 0) {
		printf( "Failed to set L2CAP security level\n");
		close(sock);
		return -1;
	}

	/* Set up destination address */
	memset(&dstaddr, 0, sizeof(dstaddr));
	dstaddr.l2_family = AF_BLUETOOTH;
	dstaddr.l2_cid = htobs(ATT_CID);
	dstaddr.l2_bdaddr_type = dst_type;
	bacpy(&dstaddr.l2_bdaddr, dst);

	printf("Connecting to device...");
	fflush(stdout);

	if (connect(sock, (struct sockaddr *) &dstaddr, sizeof(dstaddr)) < 0) {
		perror(" Failed to connect");
		close(sock);
		return -1;
	}

	printf(" Done\n");

	return sock;
}

static void *event_thread_func(void *user_data)
{
	mainloop_run();
}

int joule_init(char *adapter)
{
	int dev_id = -1;
	dev_id = hci_devid(adapter);
	if (dev_id < 0) {
		perror("Invalid adapter");
		return EXIT_FAILURE;
	}

	if (hci_devba(dev_id, &src_addr) < 0) {
		perror("Adapter not available");
		return EXIT_FAILURE;
	}

	if(mutex_init(&ready_cb_mutex) < 0)
	{
		return EXIT_FAILURE;
	}

	if(mutex_init(&read_cb_mutex) < 0)
	{
		return EXIT_FAILURE;
	}

	if(mutex_init(&write_cb_mutex) < 0)
	{
		return EXIT_FAILURE;
	}

	mainloop_init();

	if (pthread_create(&event_thread, NULL, &event_thread_func, NULL) != 0)
	{
		printf("Unable to create polling thread");
		return -1;
	}

	return EXIT_SUCCESS;
}

struct client *joule_connect(char *dest_addr,char *dest_addr_type,uint16_t mtu,char *security_level,disconnect_func_t func,void *user_data)
{
	int opt;
	int sec;
	uint8_t dst_type;
	bdaddr_t dst_addr;
	int fd;
	sigset_t mask;
	struct client *cli;

	if (strcmp(security_level, "low") == 0)
		sec = BT_SECURITY_LOW;
	else if (strcmp(security_level, "medium") == 0)
		sec = BT_SECURITY_MEDIUM;
	else if (strcmp(security_level, "high") == 0)
		sec = BT_SECURITY_HIGH;
	else
	{
		printf( "Invalid security level\n");
		return NULL;
	}

	if (mtu <= 0)
	{
		printf( "Invalid MTU: %d\n", mtu);
		return NULL;
	}

	if (mtu > UINT16_MAX)
	{
		printf( "MTU too large: %d\n", mtu);
		return NULL;
	}

	if (strcmp(dest_addr_type, "random") == 0)
		dst_type = BDADDR_LE_RANDOM;
	else if (strcmp(dest_addr_type, "public") == 0)
		dst_type = BDADDR_LE_PUBLIC;
	else
	{
		printf(
			"Allowed types: random, public\n");
		return NULL;
	}

	if (str2ba(dest_addr, &dst_addr) < 0) {
		printf( "Invalid remote address: %s\n",
				dest_addr);
		return NULL;
	}

	fd = l2cap_le_att_connect(&src_addr, &dst_addr, dst_type, sec);
	if (fd < 0)
		return NULL;

	cli = client_create(fd, mtu,func,user_data);
	if (!cli)
	{
		close(fd);
		return NULL;
	}
	return cli;
}

void joule_disconnect(struct client *cli)
{
	if(!cli)
		return;
	client_destroy(cli);
}

void joule_close()
{
	mainloop_quit();
	pthread_join(event_thread,NULL);
	/* destroy mutexes */
	mutex_destroy(&ready_cb_mutex);
	mutex_destroy(&read_cb_mutex);
	mutex_destroy(&write_cb_mutex);
	mutex_destroy(&notify_cb_mutex);
	/* clear all lists */
	oc_list_clear(ready_cb_list);
	oc_list_clear(read_cb_list);
	oc_list_clear(write_cb_list);
	oc_list_clear(notify_cb_list);

}
