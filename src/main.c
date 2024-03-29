/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "esp_log.h"
#include "nvs_flash.h"
/* BLE */
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble_spp_server.h"
#include "ble_spp_client.h"
#include "driver/uart.h"

static const char *tag = "NimBLE_SPP_BLE_PRPH";
static int ble_spp_server_gap_event(struct ble_gap_event *event, void *arg);
static uint8_t own_addr_type;
int gatt_svr_register(void);
QueueHandle_t spp_common_uart_queue = NULL;
static bool is_connect = false;
uint16_t connection_handle;
int16_t attribute_handle;
static uint16_t ble_svc_gatt_read_val_handle,ble_spp_svc_gatt_read_val_handle;

/* 16 Bit Alert Notification Service UUID */
#define BLE_SVC_ANS_UUID16                                  0x1811

/* 16 Bit Alert Notification Service Characteristic UUIDs */
#define BLE_SVC_ANS_CHR_UUID16_SUP_NEW_ALERT_CAT            0x2a47

/* 16 Bit SPP Service UUID */
#define BLE_SVC_SPP_UUID16				    0xABF0

/* 16 Bit SPP Service Characteristic UUID */
#define BLE_SVC_SPP_CHR_UUID16                              0xABF1

/* 16 Bit SPP Service UUID */
#define GATT_SPP_SVC_UUID                                  0xABF0

/* 16 Bit SPP Service Characteristic UUID */
#define GATT_SPP_CHR_UUID                                  0xABF1

volatile SemaphoreHandle_t xGuiSemaphore;

void ble_store_config_init(void);
void ble_client_my_task(void *pvParameters);

/**
 * Logs information about a connection to the console.
 */
static void
ble_spp_server_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    MODLOG_DFLT(INFO, "handle=%d our_ota_addr_type=%d our_ota_addr=",
                desc->conn_handle, desc->our_ota_addr.type);
    print_addr(desc->our_ota_addr.val);
    MODLOG_DFLT(INFO, " our_id_addr_type=%d our_id_addr=",
                desc->our_id_addr.type);
    print_addr(desc->our_id_addr.val);
    MODLOG_DFLT(INFO, " peer_ota_addr_type=%d peer_ota_addr=",
                desc->peer_ota_addr.type);
    print_addr(desc->peer_ota_addr.val);
    MODLOG_DFLT(INFO, " peer_id_addr_type=%d peer_id_addr=",
                desc->peer_id_addr.type);
    print_addr(desc->peer_id_addr.val);
    MODLOG_DFLT(INFO, " conn_itvl=%d conn_latency=%d supervision_timeout=%d "
                "encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_itvl, desc->conn_latency,
                desc->supervision_timeout,
                desc->sec_state.encrypted,
                desc->sec_state.authenticated,
                desc->sec_state.bonded);
}

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void
ble_spp_server_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (alert notifications).
     */

    memset(&fields, 0, sizeof fields);

    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(GATT_SVR_SVC_ALERT_UUID)
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return;
    }

    /* Begin advertising. */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_spp_server_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
        return;
    }
}

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * ble_spp_server uses the same callback for all connections.
 *
 * @param event                 The type of event being signalled.
 * @param ctxt                  Various information pertaining to the event.
 * @param arg                   Application-specified argument; unused by
 *                                  ble_spp_server.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int
ble_spp_server_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
	/* A new connection was established or a connection attempt failed. */
        MODLOG_DFLT(INFO, "connection %s; status=%d ",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            ble_spp_server_print_conn_desc(&desc);
	    is_connect=true;
	    connection_handle = event->connect.conn_handle;
        xTaskCreatePinnedToCore(ble_client_my_task, "myTask", 8192*2, NULL, 8, NULL,0);
        }
        MODLOG_DFLT(INFO, "\n");
        if (event->connect.status != 0) {
            /* Connection failed; resume advertising. */
            ble_spp_server_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        ble_spp_server_print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");
        connection_handle = 9999;

        /* Connection terminated; resume advertising. */
        ble_spp_server_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        MODLOG_DFLT(INFO, "connection updated; status=%d ",
                    event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        assert(rc == 0);
        ble_spp_server_print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        MODLOG_DFLT(INFO, "advertise complete; reason=%d",
                    event->adv_complete.reason);
        ble_spp_server_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    default:
    	return 0;
    }
}

static void
ble_spp_server_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void
ble_spp_server_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Printing ADDR */
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);

    MODLOG_DFLT(INFO, "Device Address: ");
    print_addr(addr_val);
    MODLOG_DFLT(INFO, "\n");
    /* Begin advertising. */
    ble_spp_server_advertise();
}

void ble_spp_server_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

/* Callback function for custom service */
static int  ble_svc_gatt_handler(uint16_t conn_handle, uint16_t attr_handle,struct ble_gatt_access_ctxt *ctxt, void *arg)
{
      switch(ctxt->op){
      case BLE_GATT_ACCESS_OP_READ_CHR:
         ESP_LOGI(tag, "Callback for read");
      break;

      case BLE_GATT_ACCESS_OP_WRITE_CHR:
	 ESP_LOGI(tag,"Data received in write event,conn_handle = %x,attr_handle = %x",conn_handle,attr_handle);
     ESP_LOGI(tag,"Some text maybe: %i: %s iiii", ctxt->om->om_len, ctxt->om->om_data);
     
      break;

      default:
         ESP_LOGI(tag, "\nDefault Callback");
      break;
      }
      return 0;
}

/* Define new custom service */
static const struct ble_gatt_svc_def new_ble_svc_gatt_defs[] = {
      {
          /*** Service: GATT */
          .type = BLE_GATT_SVC_TYPE_PRIMARY,
          .uuid = BLE_UUID16_DECLARE(BLE_SVC_ANS_UUID16),
          .characteristics = (struct ble_gatt_chr_def[]) { {
	  		/* Support new alert category */
              		.uuid = BLE_UUID16_DECLARE(BLE_SVC_ANS_CHR_UUID16_SUP_NEW_ALERT_CAT),
              		.access_cb = ble_svc_gatt_handler,
              		.val_handle = &ble_svc_gatt_read_val_handle,
              		.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
	  	},
	  	{
	  		0, /* No more characteristics */
		}
	  },
      },
      {
      	/*** Service: SPP */
          .type = BLE_GATT_SVC_TYPE_PRIMARY,
          .uuid = BLE_UUID16_DECLARE(BLE_SVC_SPP_UUID16),
          .characteristics = (struct ble_gatt_chr_def[]) { {
                        /* Support SPP service */
                        .uuid = BLE_UUID16_DECLARE(BLE_SVC_SPP_CHR_UUID16),
                        .access_cb = ble_svc_gatt_handler,
                        .val_handle = &ble_spp_svc_gatt_read_val_handle,
                        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
		},
                {
                         0, /* No more characteristics */
                }
           },
      },
      {
          0, /* No more services. */
      },
};

int gatt_svr_register(void)
{
     int rc=0;

     rc = ble_gatts_count_cfg(new_ble_svc_gatt_defs);

     if (rc != 0) {
         return rc;
     }

     rc = ble_gatts_add_svcs(new_ble_svc_gatt_defs);
     if (rc != 0) {
         return rc;
     }

     return 0;
}





void ble_client_my_task(void *pvParameters)
{
   // char myarray[13] = "thefukinserv\0";
   char myarray[2001] = "0abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "1abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "2abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "3abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "4abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "5abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "6abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "7abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "8abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "9abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "0abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "1abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "2abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "3abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "4abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "5abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "6abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "7abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "8abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi"
   "9abcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgijabcdefhgi\0";
   

    int rc;
	ESP_LOGI(tag,"My Task: BLE server send task started\n");
    if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
        ble_gattc_exchange_mtu(connection_handle, NULL, NULL);
        ESP_LOGI(tag,"MTU request sent");
        xSemaphoreGive(xGuiSemaphore);  
    }else {
            ESP_LOGI(tag,"My Task: Couldn't get semaphore for MTU exchange");
    }  
    vTaskDelay(10);
    for (;;) {
        
        int failcount = 0;       
        int taskdelay = 0;
        ESP_LOGI(tag,"My Task: Starting sending 1MB data. Taskdelay %i, MBUFs free %i.", taskdelay,os_msys_num_free() );
        for (int i = 0 ;i < 500; i++)
        {
            if (connection_handle == 9999) {
                ESP_LOGI(tag, "My task: Lost connection, quitting task.");
                vTaskDelete(NULL);
                return;
            }
            while (os_msys_num_free() < 9) {
                //ESP_LOGI(tag,"My Task: Only %i MBUFs free..waiting.", os_msys_num_free());
                vTaskDelay(1);
            }
            if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
                ESP_LOGI(tag,"My Task: Before writing %i Kb characteristic: ", i*2);
                    
                rc = ble_gattc_write_flat(connection_handle, ble_spp_svc_gatt_read_val_handle, &myarray, 2001, NULL, NULL);
                if (rc == 0){
                    ESP_LOGI(tag,"My Task: Written %i Kb data..", i*2);
                }
                else {
                    ESP_LOGI(tag,"My Task: Error after writing %i Kb characteristic: %i !!!!!", i*2, rc);
                    failcount ++;

                }
                xSemaphoreGive(xGuiSemaphore);
                //vTaskDelay(taskdelay);

            }else {
                ESP_LOGI(tag,"My Task: Couldn't get semaphore for sending data at %i Kb.", i *2);
            }  

        }
        ESP_LOGI(tag,"My Task: Done sending 1 MB of data; failcount %i", failcount);
        vTaskDelay(2000);
        
    }
     vTaskDelete(NULL);

}

void
app_main(void)
{
    int rc;

    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_nimble_hci_and_controller_init());
    xGuiSemaphore = xSemaphoreCreateMutex();
    nimble_port_init();
 

    /* Initialize uart driver and start uart task */
 //   ble_spp_uart_init();


    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb = ble_spp_server_on_reset;
    ble_hs_cfg.sync_cb = ble_spp_server_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = CONFIG_EXAMPLE_IO_TYPE;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif
#ifdef CONFIG_EXAMPLE_MITM
    ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
    ble_hs_cfg.sm_sc = 1;
#else
    ble_hs_cfg.sm_sc = 0;
#endif
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_our_key_dist = 1;
    ble_hs_cfg.sm_their_key_dist = 1;
#endif


    rc = new_gatt_svr_init();
    assert(rc == 0);

    /* Register custom service */
    rc = gatt_svr_register();
    assert(rc == 0);

    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set("nimble-ble-spp-svr");
    assert(rc == 0);

    /* XXX Need to have template for store */
    ble_store_config_init();

    nimble_port_freertos_init(ble_spp_server_host_task);
}
