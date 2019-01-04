/*
 * joule_btgatt-client.h
 *
 *  Created on: Jan 1, 2019
 *      Author: Naeem Khan
 */

#ifndef BLUEZ_TOOLS_JOULE_BTGATT_CLIENT_H_
#define BLUEZ_TOOLS_JOULE_BTGATT_CLIENT_H_

#define VALID 0
#define INVALID -1
#define REQUEST_GENERATED 0
#define REQUEST_COMPLETED 1
#define REQUEST_SUCCESS 2


typedef void (*disconnect_func_t)(int err, void *user_data);
typedef void (*notify_callback_t)(uint16_t value_handle,
					const uint8_t *value, uint16_t length,
					void *user_data);
struct client {
	int fd;
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_client *gatt;
	OC_LIST_STRUCT(service_db);
	unsigned int reliable_session_id;
};

typedef struct cb_valid_list
{
	struct cb_valid_list *next;
	int cb_valid_id;
}cb_valid_list;

typedef struct ready_cb_data
{
	uint8_t flag;
	int cb_valid_id;
}ready_cb_data;

typedef struct read_cb_data
{
	uint8_t *value;
	uint16_t length;
	uint8_t flag;
	int cb_valid_id;
}read_cb_data;

typedef struct write_cb_data
{
	uint8_t flag;
	int cb_valid_id;
}write_cb_data;

typedef struct notify_cb_data
{
	uint8_t flag;
	int cb_valid_id;
	void *ptr;
}notify_cb_data;

typedef struct chrc_db
{
	struct chrc_db *next;
	uint16_t handle;
	char uuid_str[MAX_LEN_UUID_STR];
}chrc_db;

int joule_read(struct client *cli, char *uuid_str,uint8_t *value,int *len);
int joule_write_value(struct client *cli, char *uuid_str,uint8_t *value,uint16_t length,bool response);
int joule_register_notify(struct client *cli, char *uuid_str,notify_callback_t func,void *user_data);
int joule_unregister_notify(struct client *cli, int id);
int joule_init(char *adapter);
struct client *joule_connect(char *dest_addr,char *dest_addr_type,uint16_t mtu,char *security_level,disconnect_func_t func,void *user_data);
void joule_disconnect(struct client *cli);
void joule_close();


#endif /* BLUEZ_TOOLS_JOULE_BTGATT_CLIENT_H_ */
