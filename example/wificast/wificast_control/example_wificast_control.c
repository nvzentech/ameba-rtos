#include "example_wificast_control.h"

static const char *TAG = "example_main";

static void example_control_hello_cb(u8 *src_mac, u8 *data, u16 len)
{
	u8 *print_buf = (u8 *)rtos_mem_zmalloc(len + 1);
	if (!print_buf) {
		return;
	}
	memcpy(print_buf, data, len);
	print_buf[len] = '\0';
	RTK_LOGI(TAG, MAC_FMT", recv hello: %s\n", MAC_ARG(src_mac), print_buf);
	rtos_mem_free(print_buf);
}

static void example_recv_callback(wifi_cast_node_t *pnode, unsigned char *buf, unsigned int len, signed char rssi)
{
	if (len < sizeof(struct example_frame_head)) {
		return;
	}

	struct example_frame_head *hdr = (struct example_frame_head *)buf;
	RTK_LOGI(TAG, MAC_FMT", len: %d, rssi: %d, type: %x\n", MAC_ARG(pnode->mac), len, rssi, hdr->type);
	if (hdr->type & WIFI_CAST_CONTROL_HELLO) {
		u8 *data = buf + sizeof(struct example_frame_head);
		u16 data_len = len - sizeof(struct example_frame_head);
		example_control_hello_cb(pnode->mac, data, data_len);
	}
}

static wcast_err_t example_send(u16 type, const wifi_cast_addr_t dst_mac, u8 *data, u16 data_len)
{
	wifi_cast_node_t node = {0};
	wifi_cast_node_t *dst_node;
	struct example_frame_head *hdr;
	u8 *tx_buf;
	wcast_err_t ret = WIFI_CAST_OK;

	tx_buf = (u8 *)rtos_mem_zmalloc(sizeof(struct example_frame_head) + data_len);
	if (!tx_buf) {
		RTK_LOGE(TAG, "malloc failed\n");
		return WIFI_CAST_ERR;
	}

	hdr = (struct example_frame_head *)tx_buf;
	hdr->type |= type;
	hdr->len = data_len;
	if (data) {
		memcpy(tx_buf + sizeof(struct example_frame_head), data, data_len);
	}

	wifi_cast_frame_info_t info = WIFI_CAST_FRAME_INFO_DEFAULT();
	info.ack = 0;
	memcpy(node.mac, dst_mac, ETH_ALEN);

	if ((dst_node = wifi_cast_get_node_info(&node)) != NULL) {
		ret = wifi_cast_send(dst_node, tx_buf, sizeof(struct example_frame_head) + data_len, &info);
	}
	rtos_mem_free(tx_buf);
	return ret;
}

static void example_control_hello(const char *msg)
{
	const char *hello_str = (msg && _strlen(msg) > 0) ? msg : "Hello World";
	u16 str_len = _strlen(hello_str) + 1;

	if (example_send(WIFI_CAST_CONTROL_HELLO, WIFI_CAST_BROADCAST_MAC, (u8 *)hello_str, str_len) == WIFI_CAST_OK) {
		RTK_LOGI(TAG, "%s, sent: %s\n", __func__, hello_str);
	} else {
		RTK_LOGE(TAG, "%s, send fail\n", __func__);
	}
}

#define INPUT_GPIO_PIN    PB_17

static void example_main_task(void *param)
{
	UNUSED(param);
	gpio_t gpio_input;

	rtos_time_delay_ms(2000);
	RTK_LOGI(TAG, "------------->start\n");

	wifi_cast_config_t config = WIFI_CAST_INIT_CONFIG_DEFAULT();
	WIFI_CAST_ERROR_CHECK(wifi_cast_init(&config));
	WIFI_CAST_ERROR_CHECK(wifi_cast_register_recv_cb(example_recv_callback));

	/* Initialize PB_17 as GPIO Input with Pull-Up */
	gpio_init(&gpio_input, INPUT_GPIO_PIN);
	gpio_dir(&gpio_input, PIN_INPUT);
	gpio_mode(&gpio_input, PullUp);

	while (1) {
		int pin_val = gpio_read(&gpio_input);
		RTK_LOGI(TAG, "PB_17 Input State: %s (%d)\n", pin_val ? "HIGH" : "LOW", pin_val);

		example_control_hello("Hello world data is sharing");
		rtos_time_delay_ms(1000);
	}
}

void app_example(void)
{
	if (rtos_task_create(NULL, ((const char *)"example_main_task"), example_main_task, NULL, 512 * 4, 1) != RTK_SUCCESS) {
		RTK_LOGE(TAG, "Failed to create example_main_task\n\r");
	}
}

