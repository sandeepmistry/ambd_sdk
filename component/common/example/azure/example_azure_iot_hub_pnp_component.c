#include "example_azure_iot_entry.h"
#ifdef EXAMPLE_AZURE_IOT_HUB_PNP_COMPONENT
// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <azure/az_core.h>
#include <azure/az_iot.h>

#include "FreeRTOS.h"
#include "task.h"

#include "MQTTClient.h"
#include "wifi_conf.h"
#include <sntp/sntp.h>

#include "example_azure_iot_common.h"

#include "pnp/sample_component/pnp_device_info_component.h"
#include "pnp/pnp_mqtt_message.h"
#include "pnp/pnp_protocol.h"
#include "pnp/sample_component/pnp_thermostat_component.h"

#define SAMPLE_TYPE AZIOT_HUB
#define SAMPLE_NAME AZIOT_HUB_PNP_COMPONENT_SAMPLE

#define HUB_HOSTNAME "[IoT Hub Name].azure-devices.net"	
#define HUB_DEVICE_ID "[Device ID]"

#define X509CERTIFICATE \
"-----BEGIN CERTIFICATE-----""\n" \
"MIICXzCCAgWgAwIBAgIJALmwdKzRWp0FMAoGCCqGSM49BAMCMIGLMQswCQYDVQQG""\n" \
"...""\n" \
"AiEAwIgsujX4+jkGvuJTnr74xpSWUxuJCPBUFmG/OHs4wdM=""\n" \
"-----END CERTIFICATE-----""\n"

#define X509PRIVATEKEY \
"-----BEGIN EC PARAMETERS-----""\n" \
"...""\n" \
"-----END EC PARAMETERS-----""\n" \
"-----BEGIN EC PRIVATE KEY-----""\n" \
"MHcCAQEEINO+QhrBdnUP36taC7VO6i0UNoBrbri8i8m3Jx/qIwkroAoGCCqGSM49""\n" \
"...""\n" \
"dpNJXSmXGMOJ5+6tUdkBuJ1gnxqgxj5rnw==""\n" \
"-----END EC PRIVATE KEY-----""\n"

#define DEFAULT_START_TEMP_CELSIUS 22.0
#define DOUBLE_DECIMAL_PLACE_DIGITS 2
#define PNP_RETRY_INTERVAL_SEC 1


bool is_device_operational = true;

// * PnP Values *
// The model id is the JSON document (also called the Digital Twins Model Identifier or DTMI) which
// defines the capability of your device. The functionality of the device should match what is
// described in the corresponding DTMI. Should you choose to program your own PnP capable device,
// the functionality would need to match the DTMI and you would need to update the below 'model_id'.
// Please see the sample README for more information on this DTMI.
static az_span const model_id = AZ_SPAN_LITERAL_FROM_STR("dtmi:com:example:TemperatureController;1");

// Components
static pnp_thermostat_component thermostat_1;
static pnp_thermostat_component thermostat_2;
static az_span const thermostat_1_name = AZ_SPAN_LITERAL_FROM_STR("thermostat1");
static az_span const thermostat_2_name = AZ_SPAN_LITERAL_FROM_STR("thermostat2");
static az_span const device_information_name = AZ_SPAN_LITERAL_FROM_STR("deviceInformation");
static az_span const* pnp_components[] = { &thermostat_1_name, &thermostat_2_name, &device_information_name };
static int32_t const pnp_components_num = sizeof(pnp_components) / sizeof(pnp_components[0]);

// IoT Hub Device Twin Values
static az_span const twin_reported_serial_number_property_name = AZ_SPAN_LITERAL_FROM_STR("serialNumber");
static az_span twin_reported_serial_number_property_value = AZ_SPAN_LITERAL_FROM_STR("ABCDEFG");
static az_span const twin_response_failed = AZ_SPAN_LITERAL_FROM_STR("failed");

// IoT Hub Method (Command) Values
static az_span const command_reboot_name = AZ_SPAN_LITERAL_FROM_STR("reboot");
static az_span const command_empty_response_payload = AZ_SPAN_LITERAL_FROM_STR("{}");
static char command_property_scratch_buffer[64];

// IoT Hub Telemetry Values
static az_span const telemetry_working_set_name = AZ_SPAN_LITERAL_FROM_STR("workingSet");

static iot_sample_variables az_vars;
static az_iot_hub_client hub_client;
static MQTTClient mqtt_client;
static Network mqtt_network;
static MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
static unsigned char mqtt_sendbuf[1024], mqtt_readbuf[1024];
static pnp_mqtt_message publish_message;

static void temp_controller_invoke_reboot(void);

static void create_and_configure_mqtt_client(void)
{
	int rc;

	import_user_configuration(&az_vars, IOT_SAMPLE_HUB_HOSTNAME, HUB_HOSTNAME);
	import_user_configuration(&az_vars, IOT_SAMPLE_HUB_DEVICE_ID, HUB_DEVICE_ID);
	import_user_configuration(&az_vars, IOT_SAMPLE_DEVICE_X509_CERTIFICATE, X509CERTIFICATE);
	import_user_configuration(&az_vars, IOT_SAMPLE_DEVICE_X509_PRIVATE_KEY, X509PRIVATEKEY);
	
	// Check variables set by user for purposes of running sample.
	iot_sample_check_variables(SAMPLE_TYPE, SAMPLE_NAME, &az_vars);
	
	// Initialize the hub client with the connection options.
	az_iot_hub_client_options options = az_iot_hub_client_options_default();
	options.model_id = model_id;

	// Initialize the hub client with the default connection options.
	rc = az_iot_hub_client_init(&hub_client, az_vars.hub_hostname, az_vars.hub_device_id, &options);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to initialize hub client: az_result return code 0x%08x.", rc);
	}

	NetworkInit(&mqtt_network);
	mqtt_network.clientCA = (char*)az_span_ptr(az_vars.x509_certificate);
	mqtt_network.private_key = (char*)az_span_ptr(az_vars.x509_private_key);
	mqtt_network.use_ssl = 1;

	MQTTClientInit(&mqtt_client, &mqtt_network, 30000, mqtt_sendbuf, sizeof(mqtt_sendbuf), mqtt_readbuf, sizeof(mqtt_readbuf));
  
}

static void connect_mqtt_client_to_iot_hub(void)
{
	int rc;
	char mqtt_client_username_buffer[256];
	char mqtt_client_id_buffer[128];

	// Get the MQTT client username.
	rc = az_iot_hub_client_get_user_name(&hub_client, mqtt_client_username_buffer, sizeof(mqtt_client_username_buffer), NULL);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to get MQTT client username: az_result return code 0x%08x.", rc);
	}
	// Get the MQTT client id used for the MQTT connection.
	rc = az_iot_hub_client_get_client_id(&hub_client, mqtt_client_id_buffer, sizeof(mqtt_client_id_buffer), NULL);
	if (az_result_failed(rc))
	{
	  IOT_SAMPLE_LOG_ERROR("Failed to get MQTT client id: az_result return code 0x%08x.", rc);
	}

	// Set MQTT connection options.
	connectData.clientID.cstring = mqtt_client_id_buffer;
	connectData.username.cstring = mqtt_client_username_buffer;
	connectData.cleansession = false;
	connectData.keepAliveInterval = AZ_IOT_DEFAULT_MQTT_CONNECT_KEEPALIVE_SECONDS;

	// Connect to iot hub device
	while (1) {
		do {
			if (mqtt_client.isconnected == 0) {
				  
				if (NetworkConnect(mqtt_client.ipstack, (char*)az_span_ptr(az_vars.hub_hostname), 8883) != 0) {
					break;
				}
				mqtt_printf(MQTT_INFO, "\"%s\" Connected", (char*)az_span_ptr(az_vars.hub_hostname));
  
				if (MQTTConnect(&mqtt_client, &connectData) != 0) {
					break;
				}
				mqtt_printf(MQTT_INFO, "MQTT Connected");
			}
		} while (0);
  
		if (mqtt_client.isconnected) {
			break;
		}
  
    	iot_sample_sleep_for_seconds(PNP_RETRY_INTERVAL_SEC);
	}

}

static void publish_mqtt_message(const char* topic, az_span payload, int qos)
{
	int rc;
	MQTTMessage message;
		 
	message.qos = IOT_SAMPLE_MQTT_PUBLISH_QOS;
	message.retained = 0;
	message.payload = (void*)az_span_ptr(payload);
	message.payloadlen = az_span_size(payload);
  
	if ((rc = MQTTPublish(&mqtt_client, topic, &message)) != 0)
	{
		IOT_SAMPLE_LOG_ERROR("Failed to publish message: MQTTClient return code %d", rc);
	}
}

static void receive_mqtt_message(void)
{
	static int8_t timeout_counter;
	int rc;
	Timer timer;
	TimerInit(&timer);

	IOT_SAMPLE_LOG("Waiting for command request or device twin message.\n");

	TimerCountdownMS(&timer, PNP_MQTT_TIMEOUT_RECEIVE_MS);
		  
	memset(mqtt_client.readbuf, 0, mqtt_client.readbuf_size);
	rc = cycle(&mqtt_client, &timer);
	if (*mqtt_client.readbuf == 0x0) {
		if (++timeout_counter >= PNP_MQTT_TIMEOUT_RECEIVE_MAX_MESSAGE_COUNT){
			IOT_SAMPLE_LOG("Receive message timeout expiration count of %d reached.", PNP_MQTT_TIMEOUT_RECEIVE_MAX_MESSAGE_COUNT);
			is_device_operational = false;
		}
	}else if(rc < 0){
    	IOT_SAMPLE_LOG_ERROR("Failed to receive message: MQTTClient return code %d.", rc);
		is_device_operational = false;
	}

}

static az_result append_int32_callback(az_json_writer* jw, void* value)
{
	return az_json_writer_append_int32(jw, *(int32_t*)value);
}

static az_result append_json_token_callback(az_json_writer* jw, void* value)
{
	char const* const log = "Failed to append json token callback";

	az_json_token value_token = *(az_json_token*)value;

	double value_as_double;
	int32_t string_length;

	switch (value_token.kind)
	{
		case AZ_JSON_TOKEN_NUMBER:
			IOT_SAMPLE_EXIT_IF_AZ_FAILED(az_json_token_get_double(&value_token, &value_as_double), log);
			IOT_SAMPLE_EXIT_IF_AZ_FAILED(az_json_writer_append_double(jw, value_as_double, DOUBLE_DECIMAL_PLACE_DIGITS), log);
			break;
		case AZ_JSON_TOKEN_STRING:
			IOT_SAMPLE_EXIT_IF_AZ_FAILED(
				az_json_token_get_string(
					&value_token,
					command_property_scratch_buffer,
					sizeof(command_property_scratch_buffer),
					&string_length),
				log);
			IOT_SAMPLE_EXIT_IF_AZ_FAILED(
				az_json_writer_append_string(jw, az_span_create((uint8_t*)command_property_scratch_buffer, string_length)),
				log);
			break;
		default:
			return AZ_ERROR_ITEM_NOT_FOUND;
	}

	return AZ_OK;
}

static az_result append_string_callback(az_json_writer* jw, void* value)
{
	return az_json_writer_append_string(jw, *(az_span*)value);
}

static void initialize_components(void)
{
	az_result rc;

	// Initialize thermostats 1 and 2.
	rc = pnp_thermostat_init(&thermostat_1, thermostat_1_name, DEFAULT_START_TEMP_CELSIUS);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to initialize Temperature Sensor 1: az_result return code 0x%08x.", rc);
    }

	rc = pnp_thermostat_init(&thermostat_2, thermostat_2_name, DEFAULT_START_TEMP_CELSIUS);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to initialize Temperature Sensor 2: az_result return code 0x%08x.", rc);
    }
}

static void temp_controller_build_telemetry_message(az_span payload, az_span* out_payload)
{
	int32_t working_set_ram_in_kibibytes = rand() % 128;

	pnp_build_telemetry_message(
		payload,
		telemetry_working_set_name,
		append_int32_callback,
		(void*)&working_set_ram_in_kibibytes,
		out_payload);
}

static void temp_controller_build_serial_number_reported_property(
    az_span payload,
    az_span* out_payload)
{
	pnp_build_reported_property(
		payload,
		AZ_SPAN_EMPTY,
		twin_reported_serial_number_property_name,
		append_string_callback,
		(void*)&twin_reported_serial_number_property_value,
		out_payload);
}

static void temp_controller_build_error_reported_property_with_status(
    az_span component_name,
    az_span property_name,
    az_json_reader* property_value,
    az_iot_status status,
    int32_t version,
    az_span payload,
    az_span* out_payload)
{
	pnp_build_reported_property_with_status(
		payload,
		component_name,
		property_name,
		append_json_token_callback,
		(void*)property_value,
		(int32_t)status,
		version,
		twin_response_failed,
		out_payload);
}

static bool temp_controller_process_property_update(
    az_span component_name,
    az_json_token const* property_name,
    az_json_reader const* property_value,
    int32_t version,
    az_span payload,
    az_span* out_payload)
{
	// Not implemented because no properties currently supported to update.
	(void)component_name;
	(void)property_name;
	(void)property_value;
	(void)version;
	(void)payload;
	(void)out_payload;

	return false;
}

static bool temp_controller_process_command_request(
    az_span command_name,
    az_span command_received_payload,
    az_span payload,
    az_span* out_payload,
    az_iot_status* out_status)
{
	(void)command_received_payload; // May be used in future.
	(void)payload; // May be used in future.

	if (az_span_is_content_equal(command_reboot_name, command_name))
	{
		// Invoke command.
		temp_controller_invoke_reboot();
		*out_payload = command_empty_response_payload;
		*out_status = AZ_IOT_STATUS_OK;
	}
	else // Unsupported command
	{
		*out_payload = command_empty_response_payload;
		*out_status = AZ_IOT_STATUS_NOT_FOUND;
		IOT_SAMPLE_LOG_AZ_SPAN("Command not supported on Temperature Controller:", command_name);
		return false;
	}

	return true;
}

static void property_callback(
    az_span component_name,
    az_json_token const* property_name,
    az_json_reader property_value,
    int32_t version,
    void* user_context_callback)
{
	az_result rc;

	(void)user_context_callback;

	// Get the Twin Patch topic to send a property update.
	rc = az_iot_hub_client_twin_patch_get_publish_topic(
		&hub_client,
		pnp_mqtt_get_request_id(),
		publish_message.topic,
		publish_message.topic_length,
		NULL);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to get the Twin Patch topic: az_result return code 0x%08x.", rc);
	}

	// Attempt to process property update per component until find success or exit on error.
	if (az_span_is_content_equal(thermostat_1.component_name, component_name))
	{
		if (!pnp_thermostat_process_property_update(
			&thermostat_1,
			property_name,
			&property_value,
			version,
 			publish_message.payload,
			&publish_message.out_payload))
		{
			IOT_SAMPLE_LOG_ERROR("Temperature Sensor 1 does not support writeable property `%.*s`.", az_span_size(property_name->slice), az_span_ptr(property_name->slice));

			// Build the component error message.
			pnp_thermostat_build_error_reported_property_with_status(
				component_name,
				property_name->slice,
				&property_value,
				AZ_IOT_STATUS_NOT_FOUND,
				version,
				publish_message.payload,
				&publish_message.out_payload);
		}
	}
	else if (az_span_is_content_equal(thermostat_2.component_name, component_name))
	{
		if (!pnp_thermostat_process_property_update(
			&thermostat_2,
			property_name,
			&property_value,
			version,
			publish_message.payload,
			&publish_message.out_payload))
		{
			IOT_SAMPLE_LOG_ERROR("Temperature Sensor 2 does not support writeable property `%.*s`.", az_span_size(property_name->slice), az_span_ptr(property_name->slice));

			// Build the component error message.
			pnp_thermostat_build_error_reported_property_with_status(
				component_name,
				property_name->slice,
				&property_value,
				AZ_IOT_STATUS_NOT_FOUND,
				version,
				publish_message.payload,
				&publish_message.out_payload);
		}
	}
	else if (az_span_size(component_name) == 0)
	{
		if (!temp_controller_process_property_update(
			component_name,
			property_name,
			&property_value,
			version,
			publish_message.payload,
			&publish_message.out_payload))
		{
			IOT_SAMPLE_LOG_ERROR(
				"Temperature Controller does not support writable property `%.*s`. All writeable "
				"properties are on sub-components.",
				az_span_size(property_name->slice),
				az_span_ptr(property_name->slice));

			// Build the root component error message with status.
			temp_controller_build_error_reported_property_with_status(
				component_name,
				property_name->slice,
				&property_value,
				AZ_IOT_STATUS_NOT_FOUND,
				version,
				publish_message.payload,
				&publish_message.out_payload);
		}
	}
	else
	{
		IOT_SAMPLE_LOG("No components recognized to update a property.");
		return;
	}

	// Send response. On a successfull property update above, out_payload will already be set.
	publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG_SUCCESS("Client sent reported property with status message.");
	IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);
	IOT_SAMPLE_LOG(" "); // Formatting.

	// Receive the response from the server.
	receive_mqtt_message();
}

static void handle_device_twin_message(
    MQTTMessage const* receive_message,
    az_iot_hub_client_twin_response const* twin_response)
{
	uint8_t* message_buf;
	az_span message_span;

	//If there are more then one component properties in the recieved payload, the payload buffer may be overwrited.
	//Since each property update need to publish a response to IoT Hub and need to recieve ack. 
	//Recieve ack may overwrite the payload buffer so that the rest of property update will not be completed.
	message_buf = (uint8_t*)malloc(receive_message->payloadlen * sizeof(uint8_t)+1);
	memcpy(message_buf, receive_message->payload, receive_message->payloadlen);
	message_span = az_span_create(message_buf, receive_message->payloadlen);
	
	// Invoke appropriate action per response type (3 types only).
	switch (twin_response->response_type)
	{
		// A response from a twin GET publish message with the twin document as a payload.
		case AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_TYPE_GET:
			IOT_SAMPLE_LOG("Message Type: GET");
			pnp_process_device_twin_message(message_span, false, pnp_components, pnp_components_num, property_callback, NULL);
			break;

		// An update to the desired properties with the properties as a payload.
		case AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_TYPE_DESIRED_PROPERTIES:
			IOT_SAMPLE_LOG("Message Type: Desired Properties");
			pnp_process_device_twin_message(message_span, true, pnp_components, pnp_components_num, property_callback, NULL);
			break;

		// A response from a twin reported properties publish message.
		case AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_TYPE_REPORTED_PROPERTIES:
			IOT_SAMPLE_LOG("Message Type: Reported Properties");
			break;
	}
	
	if(message_buf)
	{
		free(message_buf);
	}
}

static void handle_command_request(
    MQTTMessage const* receive_message,
    az_iot_hub_client_method_request const* command_request)
{
	az_span component_name;
	az_span command_name;
	pnp_parse_command_name(command_request->name, &component_name, &command_name);

	az_span const message_span = az_span_create((uint8_t*)receive_message->payload, receive_message->payloadlen);
	az_iot_status status = AZ_IOT_STATUS_UNKNOWN;
	
	IOT_SAMPLE_LOG_AZ_SPAN("Command :", command_request->name);

	// Invoke command and retrieve status and response payload to send to server.
	if (az_span_is_content_equal(thermostat_1.component_name, component_name))
	{
		if (pnp_thermostat_process_command_request(
			&thermostat_1,
			command_name,
			message_span,
			publish_message.payload,
			&publish_message.out_payload,
			&status))
		{
			IOT_SAMPLE_LOG_AZ_SPAN("Client invoked command on Temperature Sensor 1:", command_name);
		}
	}
	else if (az_span_is_content_equal(thermostat_2.component_name, component_name))
	{
		if (pnp_thermostat_process_command_request(
			&thermostat_2,
			command_name,
			message_span,
			publish_message.payload,
			&publish_message.out_payload,
			&status))
		{
			IOT_SAMPLE_LOG_AZ_SPAN("Client invoked command on Temperature Sensor 2:", command_name);
		}
	}
	else if (az_span_size(component_name) == 0)
	{
		if (temp_controller_process_command_request(
			command_name,
			message_span,
			publish_message.payload,
			&publish_message.out_payload,
			&status))
		{
			IOT_SAMPLE_LOG_AZ_SPAN("Client invoked command on Temperature Controller:", command_name);
		}
	}
	else
	{
		IOT_SAMPLE_LOG_AZ_SPAN("Command not supported:", command_request->name);
		publish_message.out_payload = command_empty_response_payload;
		status = AZ_IOT_STATUS_NOT_FOUND;
	}

	// Get the Methods response topic to publish the command response.
	az_result rc = az_iot_hub_client_methods_response_get_publish_topic(
		&hub_client,
		command_request->request_id,
		(uint16_t)status,
		publish_message.topic,
		publish_message.topic_length,
		NULL);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to get the Methods response topic: az_result return code 0x%08x.", rc);
	}

	// Publish the command response.
	publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG_SUCCESS("Client published command response.");
	IOT_SAMPLE_LOG("Status: %d", status);
	IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);
}

static void on_message_received(
    char* topic,
    int topic_len,
    MQTTMessage const* receive_message)
{
	az_result rc;

	az_span const topic_span = az_span_create((uint8_t*)topic, topic_len);
	az_span const message_span = az_span_create((uint8_t*)receive_message->payload, receive_message->payloadlen);

	az_iot_hub_client_twin_response twin_response;
	az_iot_hub_client_method_request command_request;

	// Parse the incoming message topic and handle appropriately.
	rc = az_iot_hub_client_twin_parse_received_topic(&hub_client, topic_span, &twin_response);
	if (az_result_succeeded(rc))
	{
		IOT_SAMPLE_LOG_SUCCESS("Client received a valid topic response.");
		IOT_SAMPLE_LOG_AZ_SPAN("Topic:", topic_span);
		IOT_SAMPLE_LOG_AZ_SPAN("Payload:", message_span);
		IOT_SAMPLE_LOG("Status: %d", twin_response.status);

		handle_device_twin_message(receive_message, &twin_response);
	}
	else
	{
		rc = az_iot_hub_client_methods_parse_received_topic(&hub_client, topic_span, &command_request);
		if (az_result_succeeded(rc))
		{
			IOT_SAMPLE_LOG_SUCCESS("Client received a valid topic response.");
			IOT_SAMPLE_LOG_AZ_SPAN("Topic:", topic_span);
			IOT_SAMPLE_LOG_AZ_SPAN("Payload:", message_span);

			handle_command_request(receive_message, &command_request);
		}
		else
		{
			IOT_SAMPLE_LOG_ERROR("Message from unknown topic: az_result return code 0x%08x.", rc);
			IOT_SAMPLE_LOG_AZ_SPAN("Topic:", topic_span);
		}
	}
}

static void messageArrived(MessageData* data)
{
	IOT_SAMPLE_LOG_SUCCESS("Client received a message from the service.");

	on_message_received(data->topicName->lenstring.data, data->topicName->lenstring.len, data->message);

	IOT_SAMPLE_LOG(" "); // Formatting
}

static void subscribe_mqtt_client_to_iot_hub_topics(void)
{
	int rc;

	// Messages received on the Methods topic will be commands to be invoked.
	if ((rc = MQTTSubscribe(&mqtt_client, AZ_IOT_HUB_CLIENT_METHODS_SUBSCRIBE_TOPIC, QOS1, messageArrived)) != SUCCESS) {
		IOT_SAMPLE_LOG_ERROR("Failed to subscribe to the Methods topic: MQTTClient return code %d.", rc);
	}
  

	// Messages received on the Twin Patch topic will be updates to the desired properties.
	if ((rc = MQTTSubscribe(&mqtt_client, AZ_IOT_HUB_CLIENT_TWIN_PATCH_SUBSCRIBE_TOPIC, QOS1, messageArrived)) != SUCCESS) {
		IOT_SAMPLE_LOG_ERROR("Failed to subscribe to the Twin Patch topic: MQTTClient return code %d.", rc);
	}
  

	// Messages received on Twin Response topic will be response statuses from the server.
	if ((rc = MQTTSubscribe(&mqtt_client, AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_SUBSCRIBE_TOPIC, QOS1, messageArrived)) != SUCCESS) {
		IOT_SAMPLE_LOG_ERROR("Failed to subscribe to the Twin Response topic: MQTTClient return code %d.", rc);
	}
}

static void send_telemetry_messages(void)
{
	// Temperature Sensor 1
	// Get the Telemetry topic to publish the telemetry message.
	pnp_telemetry_get_publish_topic(
		&hub_client,
		NULL,
		thermostat_1.component_name,
		publish_message.topic,
		publish_message.topic_length,
		NULL);

	// Build the Telemetry message.
	pnp_thermostat_build_telemetry_message(&thermostat_1, publish_message.payload, &publish_message.out_payload);

	// Publish the telemetry message.
	publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG_SUCCESS("Client published the Telemetry message for Temperature Sensor 1.");
	IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);

	// Temperature Sensor 2
	// Get the Telemetry topic to publish the telemetry message.
	pnp_telemetry_get_publish_topic(
		&hub_client,
		NULL,
		thermostat_2.component_name,
		publish_message.topic,
		publish_message.topic_length,
		NULL);

	// Build the Telemetry message.
	pnp_thermostat_build_telemetry_message(&thermostat_2, publish_message.payload, &publish_message.out_payload);

	// Publish the telemetry message.
	publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG_SUCCESS("Client published the Telemetry message for Temperature Sensor 2.");
	IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);

	// Temperature Controller
	// Get the Telemetry topic to publish the telemetry message.
	pnp_telemetry_get_publish_topic(&hub_client, NULL, AZ_SPAN_EMPTY, publish_message.topic, publish_message.topic_length, NULL);

	// Build the Telemetry message.
	temp_controller_build_telemetry_message(publish_message.payload, &publish_message.out_payload);

	// Publish the Telemetry message.
	publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG_SUCCESS("Client published the Telemetry message for the Temperature Controller.");
	IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);
}

static void receive_messages(void)
{
	// Continue to receive commands or device twin messages while device is operational.
	while (is_device_operational)
	{
		az_result rc;

		// Send max temp for each thermostat since boot, if needed.
		if (thermostat_1.send_maximum_temperature_property)
		{
			// Get the Twin Patch topic to send a reported property update.
			rc = az_iot_hub_client_twin_patch_get_publish_topic(
				&hub_client,
				pnp_mqtt_get_request_id(),
				publish_message.topic,
				publish_message.topic_length,
				NULL);
			if (az_result_failed(rc))
			{
				IOT_SAMPLE_LOG_ERROR("Failed to get the Twin Patch topic: az_result return code 0x%08x.", rc);
			}

			// Build the maximum temperature reported property message.
			az_span property_name;
			pnp_thermostat_build_maximum_temperature_reported_property(&thermostat_1, publish_message.payload, &publish_message.out_payload, &property_name);

			// Publish the maximum temperature reported property update.
			publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
			IOT_SAMPLE_LOG_SUCCESS("Client sent Temperature Sensor 1 the `%.*s` reported property message.", az_span_size(property_name), az_span_ptr(property_name));
				
			IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);
			IOT_SAMPLE_LOG(" "); // Formatting

			thermostat_1.send_maximum_temperature_property = false; // Only send again if new max set.

			// Receive the response from the server.
			receive_mqtt_message();
		}

		if (thermostat_2.send_maximum_temperature_property)
		{
			// Get the Twin Patch topic to send a reported property update.
			rc = az_iot_hub_client_twin_patch_get_publish_topic(
				&hub_client,
				pnp_mqtt_get_request_id(),
				publish_message.topic,
				publish_message.topic_length,
				NULL);
			if (az_result_failed(rc))
			{
				IOT_SAMPLE_LOG_ERROR("Failed to get the Twin Patch topic: az_result return code 0x%08x.", rc);
			}

			// Build the maximum temperature reported property message.
			az_span property_name;
			pnp_thermostat_build_maximum_temperature_reported_property(&thermostat_2, publish_message.payload, &publish_message.out_payload, &property_name);

			// Publish the maximum temperature reported property update.
			publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);

			IOT_SAMPLE_LOG_SUCCESS("Client sent Temperature Sensor 2 the `%.*s` reported property message.", az_span_size(property_name), az_span_ptr(property_name));
			IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);
			IOT_SAMPLE_LOG(" "); // Formatting

			thermostat_2.send_maximum_temperature_property = false; // Only send again if new max set.

			// Receive the response from the server.
			receive_mqtt_message();
		}

		// Send telemetry messages. No response requested from server.
		send_telemetry_messages();
		IOT_SAMPLE_LOG(" "); // Formatting.

		// Wait for any server-initiated messages.
		receive_mqtt_message();
	}
}

static void send_device_info(void)
{
	// Get the Twin Patch topic to send a reported property update.
	az_result rc = az_iot_hub_client_twin_patch_get_publish_topic(
		&hub_client,
		pnp_mqtt_get_request_id(),
		publish_message.topic,
		publish_message.topic_length,
		NULL);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to get the Twin Patch topic: az_result return code 0x%08x.", rc);
    }

	// Build the device info reported property message.
	pnp_device_info_build_reported_property(publish_message.payload, &publish_message.out_payload);

	// Publish the device info reported property update.
	publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG_SUCCESS("Client sent reported property message for device info.");
	IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);
	IOT_SAMPLE_LOG(" "); // Formatting

	// Receive the response from the server.
	receive_mqtt_message();
}

static void send_serial_number(void)
{
	// Get the Twin Patch topic to send a reported property update.
	az_result rc = az_iot_hub_client_twin_patch_get_publish_topic(
		&hub_client,
		pnp_mqtt_get_request_id(),
      publish_message.topic,
      publish_message.topic_length,
      NULL);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to get the Twin Patch topic: az_result return code 0x%08x.", rc);
    }

	// Build the serial number reported property message.
	temp_controller_build_serial_number_reported_property(publish_message.payload, &publish_message.out_payload);

	// Publish the serial number reported property update.
	publish_mqtt_message(publish_message.topic, publish_message.out_payload, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG_SUCCESS(
		"Client sent `%.*s` reported property message.",
		az_span_size(twin_reported_serial_number_property_name),
		az_span_ptr(twin_reported_serial_number_property_name));
	IOT_SAMPLE_LOG_AZ_SPAN("Payload:", publish_message.out_payload);
	IOT_SAMPLE_LOG(" "); // Formatting

	// Receive the response from the server.
	receive_mqtt_message();
}

static void request_device_twin_document(void)
{
	IOT_SAMPLE_LOG("Client requesting device twin document from service.");

	// Get the Twin Document topic to publish the twin document request.
	az_result rc = az_iot_hub_client_twin_document_get_publish_topic(
		&hub_client,
		pnp_mqtt_get_request_id(),
		publish_message.topic,
		publish_message.topic_length,
		NULL);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to get the Twin Document topic: az_result return code 0x%08x.", rc);
    }

	// Publish the twin document request.
	publish_mqtt_message(publish_message.topic, AZ_SPAN_EMPTY, IOT_SAMPLE_MQTT_PUBLISH_QOS);
	IOT_SAMPLE_LOG(" "); // Formatting

	// Receive the response from the server.
	receive_mqtt_message();
}

static void disconnect_mqtt_client_from_iot_hub(void)
{
	int rc = MQTTDisconnect(&mqtt_client);
	if (rc != SUCCESS)
	{
		IOT_SAMPLE_LOG_ERROR("Failed to disconnect MQTT client: MQTTClient return code %d.", rc);
	}
	mqtt_network.disconnect(&mqtt_network);
}

static void temp_controller_invoke_reboot(void)
{
	IOT_SAMPLE_LOG("Client invoking reboot command on Temperature Controller.\n");
	IOT_SAMPLE_LOG("Client rebooting.\n");

	disconnect_mqtt_client_from_iot_hub();
	IOT_SAMPLE_LOG_SUCCESS("Client disconnected from IoT Hub.");

	create_and_configure_mqtt_client();
	IOT_SAMPLE_LOG_SUCCESS("Client created and configured.");

	connect_mqtt_client_to_iot_hub();
	IOT_SAMPLE_LOG_SUCCESS("Client connected to IoT Hub.");

	subscribe_mqtt_client_to_iot_hub_topics();
	IOT_SAMPLE_LOG_SUCCESS("Client subscribed to IoT Hub topics.");

	// Initializations
	az_result rc = pnp_mqtt_message_init(&publish_message);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to initialize pnp_mqtt_message: az_result return code 0x%08x.", rc);
	}
	initialize_components();
	IOT_SAMPLE_LOG_SUCCESS("Client initialized all components.");

	// Messaging
	send_device_info();
	send_serial_number();
	request_device_twin_document();
}

/*
 * This sample extends the IoT Hub Plug and Play Sample above to mimic a Temperature Controller
 * and connects the IoT Plug and Play enabled device (the Temperature Controller) with the Digital
 * Twin Model ID (DTMI). If a timeout occurs while waiting for a message from the Azure IoT
 * Explorer, the sample will continue. If PNP_MQTT_TIMEOUT_RECEIVE_MAX_COUNT timeouts occur
 * consecutively, the sample will disconnect. X509 self-certification is used.
 *
 * This Temperature Controller is made up of the following components:
 * - Device Info
 * - Temperature Sensor 1
 * - Temperature Sensor 2
 *
 * To interact with this sample, you must use the Azure IoT Explorer. The capabilities are Device
 * Twin, Direct Method (Command), and Telemetry:
 *
 * Device Twin: The following device twin properties are supported in this sample:
 *
 * Temperature Controller:
 * - A reported property named `serialNumber` with a `string` value for the device serial number.
 *
 * Device Info:
 * - A reported property named `manufacturer` with a `string` value for the name of the device
 * manufacturer.
 * - A reported property named `model` with a `string` value for the name of the device model.
 * - A reported property named `swVersion` with a `string` value for the software version running on
 * the device.
 * - A reported property named `osName` with a `string` value for the name of the operating system
 * running on the device.
 * - A reported property named `processorArchitecture` with a `string` value for the name of the
 * device architecture.
 * - A reported property named `processorManufacturer` with a `string` value for the name of the
 * device's processor manufacturer.
 * - A reported property named `totalStorage` with a `double` value for the total storage in KiB on
 * the device.
 * - A reported property named `totalMemory` with a `double` value for the total memory in KiB on
 * the device.
 *
 * Temperature Sensor:
 * - A desired property named `targetTemperature` with a `double` value for the desired temperature.
 * - A reported property named `maxTempSinceLastReboot` with a `double` value for the highest
 * temperature reached since boot.
 *
 * On initial bootup of the device, the sample will send the Temperature Controller reported
 * properties to the service. You will see the following in the device twin JSON.
 *   {
 *     "properties": {
 *       "reported": {
 *         "manufacturer": "Sample-Manufacturer",
 *         "model": "pnp-sample-Model-123",
 *         "swVersion": "1.0.0.0",
 *         "osName": "Contoso",
 *         "processorArchitecture": "Contoso-Arch-64bit",
 *         "processorManufacturer": "Processor Manufacturer(TM)",
 *         "totalStorage": 1024,
 *         "totalMemory": 128,
 *         "serialNumber": "ABCDEFG",
 *       }
 *     }
 *   }
 *
 * To send a Temperature Sensor device twin desired property message, select your device's Device
 * Twin tab in the Azure IoT Explorer. Add the property targetTemperature along with a corresponding
 * value to the corresponding thermostat in the desired section of the JSON. Select Save to update
 * the twin document and send the twin message to the device.
 *   {
 *     "properties": {
 *       "desired": {
 *         "thermostat1": {
 *           "targetTemperature": 34.8
 *         },
 *         "thermostat2": {
 *           "targetTemperature": 68.5
 *         }
 *       }
 *     }
 *   }
 *
 * Upon receiving a desired property message, the sample will update the twin property locally and
 * send a reported property of the same name back to the service. This message will include a set of
 * "ack" values: `ac` for the HTTP-like ack code, `av` for ack version of the property, and an
 * optional `ad` for an ack description.
 *   {
 *     "properties": {
 *       "reported": {
 *         "thermostat1": {
 *           "__t": "c",
 *           "maxTempSinceLastReboot": 38.2,
 *           "targetTemperature": {
 *             "value": 34.8,
 *             "ac": 200,
 *             "av": 27,
 *             "ad": "success"
 *           }
 *         },
 *         "thermostat2": {
 *           "__t": "c",
 *           "maxTempSinceLastReboot": 69.1,
 *           "targetTemperature": {
 *             "value": 68.5,
 *             "ac": 200,
 *             "av": 28,
 *             "ad": "success"
 *           }
 *         }
 *       }
 *     }
 *   }
 *
 * Direct Method (Command): Two device commands are supported in this sample: `reboot` and
 * `getMaxMinReport`. If any other commands are attempted to be invoked, the log will report the
 * command is not found. To invoke a command, select your device's Direct Method tab in the Azure
 * IoT Explorer.
 *
 * - To invoke `reboot` on the Temperature Controller, enter the command name `reboot`. Select
 * Invoke method.
 * - To invoke `getMaxMinReport` on Temperature Sensor 1, enter the command name
 * `thermostat1/getMaxMinReport` along with a payload using an ISO8601 time format. Select Invoke
 * method.
 * - To invoke `getMaxMinReport` on Temperature Sensor 2, enter the command name
 * `thermostat2/getMaxMinReport` along with a payload using an ISO8601 time format. Select Invoke
 * method.
 *
 *   "2020-08-18T17:09:29-0700"
 *
 * The command will send back to the service a response containing the following JSON payload with
 * updated values in each field:
 *   {
 *     "maxTemp": 74.3,
 *     "minTemp": 65.2,
 *     "avgTemp": 68.79,
 *     "startTime": "2020-08-18T17:09:29-0700",
 *     "endTime": "2020-08-18T17:24:32-0700"
 *   }
 *
 * Telemetry: The Temperature Controller sends a JSON message with the property name `workingSet`
 * and a `double` value for the current working set of the device memory in KiB. Also, each
 * Temperature Sensor sends a JSON message with the property name `temperature` and a `double`
 * value for the current temperature.
 */
static void example_azure_iot_hub_pnp_component_thread(void* param)
{
	while (wifi_is_ready_to_transceive(RTW_STA_INTERFACE) != SUCCESS) {
		vTaskDelay(1000);
	}

	//for directed method query for ISO8061 time
	sntp_init();

	create_and_configure_mqtt_client();
	IOT_SAMPLE_LOG_SUCCESS("Client created and configured.");

	connect_mqtt_client_to_iot_hub();
	IOT_SAMPLE_LOG_SUCCESS("Client connected to IoT Hub.");

	subscribe_mqtt_client_to_iot_hub_topics();
	IOT_SAMPLE_LOG_SUCCESS("Client subscribed to IoT Hub topics.");

	// Initializations
	az_result rc = pnp_mqtt_message_init(&publish_message);
	if (az_result_failed(rc))
	{
		IOT_SAMPLE_LOG_ERROR("Failed to initialize pnp_mqtt_message: az_result return code 0x%08x.", rc);
	}
	initialize_components();
	IOT_SAMPLE_LOG_SUCCESS("Client initialized all components.");

	// Messaging
	send_device_info();
	send_serial_number();
	request_device_twin_document();
	receive_messages();

	disconnect_mqtt_client_from_iot_hub();
	IOT_SAMPLE_LOG_SUCCESS("Client disconnected from IoT Hub.");
  
	vTaskDelete(NULL);
}


void example_azure_iot_hub_pnp_component(void)
{
	if(xTaskCreate(example_azure_iot_hub_pnp_component_thread, ((const char*)"example_azure_iot_hub_pnp_component_thread"), 8000, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
		printf("\n\r%s xTaskCreate(example_azure_iot_hub_pnp_component_thread) failed", __FUNCTION__);
}


#endif
