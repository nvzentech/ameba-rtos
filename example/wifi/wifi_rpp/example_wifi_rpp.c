#include "example_wifi_rpp.h"
#include "wifi_api.h"
#include "wifi_api_wtn.h"
#include "lwip_netconf.h"
#include <lwip/sockets.h>
#include "wtn_app_socket.h"
#include "gpio_api.h"

#define INPUT_GPIO_PIN PB_18

extern void wtn_zrpp_start(void);
int ble_wifimate_device_main(uint8_t enable, uint16_t timeout);

#define RPP_DATA_PORT 5000

/* Task to receive data from another board */
static void wifi_rpp_rx_task(void *param)
{
	(void)param;
	int server_fd = -1;
	struct sockaddr_in server_addr, client_addr;
	socklen_t addr_len = sizeof(client_addr);
	char rx_buf[256];
	int recv_bytes;

	/* 1. Wait until R-Mesh ZRPP connects and DHCP assigns a valid IP address */
	u8 *ip = (u8 *)lwip_get_ip(NETIF_WLAN_STA_INDEX);
	u8 invalid_ip[4] = {0};
	while (memcmp(ip, invalid_ip, 4) == 0) {
		rtos_time_delay_ms(1000);
		ip = (u8 *)lwip_get_ip(NETIF_WLAN_STA_INDEX);
	}
	RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Mesh RX] Network ready. IP: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

	/* 2. Create UDP socket */
	server_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (server_fd < 0) {
		RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[Mesh RX] Socket creation failed\n");
		rtos_task_delete(NULL);
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(RPP_DATA_PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[Mesh RX] Bind failed\n");
		closesocket(server_fd);
		rtos_task_delete(NULL);
	}

	RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Mesh RX] Successfully listening for UDP data on port %d...\n", RPP_DATA_PORT);

	/* 3. Receive data loop */
	while (1) {
		addr_len = sizeof(client_addr);
		recv_bytes = recvfrom(server_fd, rx_buf, sizeof(rx_buf) - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
		if (recv_bytes > 0) {
			rx_buf[recv_bytes] = '\0';
			u8 *from_ip = (u8 *)&client_addr.sin_addr.s_addr;

			/* Skip messages from own IP (own broadcasts) */
			ip = (u8 *)lwip_get_ip(NETIF_WLAN_STA_INDEX);
			if (memcmp(from_ip, ip, 4) == 0) {
				continue;
			}

			/* Process ACK messages (Skip sending ACK back to avoid loops) */
			if (strncmp(rx_buf, "ACK", 3) == 0) {
				RTK_LOGS(NOTAG, RTK_LOG_INFO, "--- [Mesh RX] Received ACK from %d.%d.%d.%d ---\n",
					from_ip[0], from_ip[1], from_ip[2], from_ip[3]);
				continue;
			}

			/* Regular data from another board */
			RTK_LOGS(NOTAG, RTK_LOG_INFO, "\n<<< [Mesh RX] Received Message <<<\n");
			RTK_LOGS(NOTAG, RTK_LOG_INFO, "    From IP: %d.%d.%d.%d\n", from_ip[0], from_ip[1], from_ip[2], from_ip[3]);
			RTK_LOGS(NOTAG, RTK_LOG_INFO, "    Payload: \"%s\" (%d bytes)\n", rx_buf, recv_bytes);
			RTK_LOGS(NOTAG, RTK_LOG_INFO, "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n");

			rtos_time_delay_ms(500);
			
			/* Send ACK back to the sender on RPP_DATA_PORT */
			const char *ack_msg = "ACK OK";
			client_addr.sin_port = htons(RPP_DATA_PORT);
			sendto(server_fd, ack_msg, strlen(ack_msg), 0,
				(struct sockaddr *)&client_addr, sizeof(client_addr));
			RTK_LOGS(NOTAG, RTK_LOG_INFO, ">>> [Mesh TX] Sent ACK back to %d.%d.%d.%d >>>\n",
				from_ip[0], from_ip[1], from_ip[2], from_ip[3]);


			//	rtos_time_delay_ms(1000);


		}
	}
}

static void print_mesh_topology(void)
{
	struct rtw_rmesh_node_info self_info = {0};
	struct rtw_rmesh_node_info father_info = {0};
	struct rtw_rmesh_node_info root_info = {0};

	u8 *ip = (u8 *)lwip_get_ip(NETIF_WLAN_STA_INDEX);

	RTK_LOGS(NOTAG, RTK_LOG_INFO, "\n========== MESH STATUS & TOPOLOGY ==========\n");

	struct rtw_wifi_setting setting;
	if (wifi_get_setting(0, &setting) == RTK_SUCCESS) {
		RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Network] SSID: %s | IP: %d.%d.%d.%d\n", 
				 setting.ssid, ip[0], ip[1], ip[2], ip[3]);
	} else {
		RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Network] IP: %d.%d.%d.%d (SSID Unknown)\n", 
				 ip[0], ip[1], ip[2], ip[3]);
	}

	if (wifi_rmesh_get_node_info(RMESH_SELF_NODE, &self_info) == RTK_SUCCESS) {
		const char *role = "Child";
		if (self_info.layer == 1) role = "Root";
		else if (wifi_rmesh_get_child_num() > 0) role = "Father (Intermediate)";
		
		RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Self] Role: %s, Layer: %d, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", 
				 role, self_info.layer,
				 self_info.mac[0], self_info.mac[1], self_info.mac[2], 
				 self_info.mac[3], self_info.mac[4], self_info.mac[5]);
	}

	if (wifi_rmesh_get_node_info(RMESH_FATHER_NODE, &father_info) == RTK_SUCCESS) {
		RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Uplink/Father] Layer: %d, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", 
				 father_info.layer,
				 father_info.mac[0], father_info.mac[1], father_info.mac[2], 
				 father_info.mac[3], father_info.mac[4], father_info.mac[5]);
	} else {
		RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Uplink/Father] None (Direct to Router or Unconnected)\n");
	}

	if (wifi_rmesh_get_node_info(RMESH_ROOT_NODE, &root_info) == RTK_SUCCESS) {
		RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Root Node] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", 
				 root_info.mac[0], root_info.mac[1], root_info.mac[2], 
				 root_info.mac[3], root_info.mac[4], root_info.mac[5]);
	}

	u8 child_num = wifi_rmesh_get_child_num();
	if (child_num > 0) {
		struct rtw_rmesh_node_info children[4];
		u8 max_children = sizeof(children) / sizeof(children[0]);
		u8 req_children = (child_num > max_children) ? max_children : child_num;
		if (wifi_rmesh_get_child_info_list(&req_children, children) == RTK_SUCCESS) {
			for (u8 i = 0; i < req_children; i++) {
				RTK_LOGS(NOTAG, RTK_LOG_INFO, "[Downlink/Child %d] Layer: %d, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", 
						 i+1, children[i].layer,
						 children[i].mac[0], children[i].mac[1], children[i].mac[2], 
						 children[i].mac[3], children[i].mac[4], children[i].mac[5]);
			}
		}
	}
	RTK_LOGS(NOTAG, RTK_LOG_INFO, "============================================\n");
}

/* Task to send data to another board */
static void wifi_rpp_tx_task(void *param)
{
	(void)param;
	int client_fd = -1;
	struct sockaddr_in dest_addr;
	int broadcast_opt = 1;
	char tx_buf[128];

	/* 1. Wait until R-Mesh ZRPP connects and DHCP assigns a valid IP address */
	u8 *ip = (u8 *)lwip_get_ip(NETIF_WLAN_STA_INDEX);
	u8 invalid_ip[4] = {0};
	while (memcmp(ip, invalid_ip, 4) == 0) {
		rtos_time_delay_ms(1000);
		ip = (u8 *)lwip_get_ip(NETIF_WLAN_STA_INDEX);
	}

	client_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (client_fd < 0) {
		RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[Mesh TX] Socket creation failed\n");
		rtos_task_delete(NULL);
	}

	/* 2. Enable broadcast to reach other mesh boards */
	setsockopt(client_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt));

	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(RPP_DATA_PORT);
	dest_addr.sin_addr.s_addr = inet_addr("192.168.1.10"); /* Or target board's IP */

	gpio_t gpio_input;

	/* 3. Initialize GPIO Input with Pull-Down */
	gpio_init(&gpio_input, INPUT_GPIO_PIN);
	gpio_dir(&gpio_input, PIN_INPUT);
	gpio_mode(&gpio_input, PullDown);

	/* 4. Send data loop - GPIO checked every iteration */
	while (1) {
		int pin_val = gpio_read(&gpio_input);
		print_mesh_topology();

		if (pin_val == 1) {
			snprintf(tx_buf, sizeof(tx_buf), "Hello, this is data from board 3 ");

			RTK_LOGS(NOTAG, RTK_LOG_INFO, "\n>>> [Mesh TX] GPIO Triggered! Sending Message >>>\n");
			RTK_LOGS(NOTAG, RTK_LOG_INFO, "    To IP: 192.168.1.10 (Port %d)\n", RPP_DATA_PORT);
			RTK_LOGS(NOTAG, RTK_LOG_INFO, "    Payload: \"%s\"\n", tx_buf);

			sendto(client_fd, tx_buf, strlen(tx_buf), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
			
			RTK_LOGS(NOTAG, RTK_LOG_INFO, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
		}

		rtos_time_delay_ms(10000); /* 10 second delay */
	}
}

void wifi_rpp_task(void *param)
{
	UNUSED(param);

	/* 0. Disable Auto-Reconnect so it forgets old Wi-Fi passwords in memory 
	 * and ONLY joins the mesh network via ZRPP */
	wifi_set_autoreconnect(1);

	/* 1. Start ZRPP provisioning protocol task */
	wtn_zrpp_start();

	/* 2. Start BLE WiFiMate for device provisioning */
	ble_wifimate_device_main(1, 60);

	/* 3. Create Receiver Task (always runs - all boards can receive) */
	if (rtos_task_create(NULL, "wifi_rpp_rx", wifi_rpp_rx_task, NULL, 1024 * 3, 1) != RTK_SUCCESS) {
		RTK_LOGS(NOTAG, RTK_LOG_ERROR, "Create RPP RX task failed\n");
	}

	/* 4. Create Transmitter Task (checks GPIO inside to decide send or idle) */
	if (rtos_task_create(NULL, "wifi_rpp_tx", wifi_rpp_tx_task, NULL, 1024 * 3, 1) != RTK_SUCCESS) {
		RTK_LOGS(NOTAG, RTK_LOG_ERROR, "Create RPP TX task failed\n");
	}

	rtos_task_delete(NULL);
}

void example_wifi_rpp(void)
{
	if (rtos_task_create(NULL, ((const char *)"wifi_rpp_task"), wifi_rpp_task, NULL, 1024 * 4, 1) != RTK_SUCCESS) {
		RTK_LOGS(NOTAG, RTK_LOG_ERROR, "\n\r[%s] Create wifi provisioning task failed", __FUNCTION__);
	}
}