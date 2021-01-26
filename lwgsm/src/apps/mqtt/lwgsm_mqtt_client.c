/**
 * \file            lwgsm_mqtt_client.c
 * \brief           MQTT client
 */

/*
 * Copyright (c) 2020 Tilen MAJERLE
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of LwGSM - Lightweight GSM-AT library.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 * Version:         v0.1.0
 */
#include "lwgsm/apps/lwgsm_mqtt_client.h"
#include "lwgsm/lwgsm_mem.h"
#include "lwgsm/lwgsm_pbuf.h"

/**
 * \brief           MQTT client connection
 */
typedef struct lwgsm_mqtt_client {
    lwgsm_conn_p conn;                          /*!< Active used connection for MQTT */
    const lwgsm_mqtt_client_info_t* info;       /*!< Connection info */
    lwgsm_mqtt_state_t conn_state;              /*!< MQTT connection state */

    uint32_t poll_time;                         /*!< Poll time, increased every 500ms */

    lwgsm_mqtt_evt_t evt;                       /*!< MQTT event callback */
    lwgsm_mqtt_evt_fn evt_fn;                   /*!< Event callback function */

    lwgsm_buff_t tx_buff;                       /*!< Buffer for raw output data to transmit */

    uint8_t is_sending;                         /*!< Flag if we are sending data currently */
    uint32_t sent_total;                        /*!< Total number of bytes sent so far on connection */
    uint32_t written_total;                     /*!< Total number of bytes written into send buffer and queued for send */

    uint16_t last_packet_id;                    /*!< Packet ID used on last packet */

    lwgsm_mqtt_request_t requests[LWGSM_CFG_MQTT_MAX_REQUESTS]; /*!< List of requests */

    uint8_t* rx_buff;                           /*!< Raw RX buffer */
    size_t rx_buff_len;                         /*!< Length of raw RX buffer */

    uint8_t parser_state;                       /*!< Incoming data parser state */
    uint8_t msg_hdr_byte;                       /*!< Incoming message header byte */
    uint32_t msg_rem_len;                       /*!< Remaining length value of current message */
    uint8_t msg_rem_len_mult;                   /*!< Multiplier for remaining length */
    uint32_t msg_curr_pos;                      /*!< Current buffer write pointer */

    void* arg;                                  /*!< User argument */
} lwgsm_mqtt_client_t;

/* Tracing debug message */
#define LWGSM_CFG_DBG_MQTT_TRACE                  (LWGSM_CFG_DBG_MQTT | LWGSM_DBG_TYPE_TRACE)
#define LWGSM_CFG_DBG_MQTT_STATE                  (LWGSM_CFG_DBG_MQTT | LWGSM_DBG_TYPE_STATE)
#define LWGSM_CFG_DBG_MQTT_TRACE_WARNING          (LWGSM_CFG_DBG_MQTT | LWGSM_DBG_TYPE_TRACE | LWGSM_DBG_LVL_WARNING)

static lwgsmr_t   mqtt_conn_cb(lwgsm_evt_t* evt);
static void     send_data(lwgsm_mqtt_client_p client);

/**
 * \brief           List of MQTT message types
 */
typedef enum {
    MQTT_MSG_TYPE_CONNECT =     0x01,           /*!< Client requests a connection to a server */
    MQTT_MSG_TYPE_CONNACK =     0x02,           /*!< Acknowledge connection request */
    MQTT_MSG_TYPE_PUBLISH =     0x03,           /*!< Publish message */
    MQTT_MSG_TYPE_PUBACK =      0x04,           /*!< Publish acknowledgement */
    MQTT_MSG_TYPE_PUBREC =      0x05,           /*!< Public received */
    MQTT_MSG_TYPE_PUBREL =      0x06,           /*!< Publish release */
    MQTT_MSG_TYPE_PUBCOMP =     0x07,           /*!< Publish complete */
    MQTT_MSG_TYPE_SUBSCRIBE =   0x08,           /*!< Subscribe to topics */
    MQTT_MSG_TYPE_SUBACK =      0x09,           /*!< Subscribe acknowledgement */
    MQTT_MSG_TYPE_UNSUBSCRIBE = 0x0A,           /*!< Unsubscribe from topics */
    MQTT_MSG_TYPE_UNSUBACK =    0x0B,           /*!< Unsubscribe acknowledgement */
    MQTT_MSG_TYPE_PINGREQ =     0x0C,           /*!< Ping request */
    MQTT_MSG_TYPE_PINGRESP =    0x0D,           /*!< Ping response */
    MQTT_MSG_TYPE_DISCONNECT =  0x0E,           /*!< Disconnect notification */
} mqtt_msg_type_t;

/* List of flags for CONNECT message type */
#define MQTT_FLAG_CONNECT_USERNAME      0x80    /*!< Packet contains username */
#define MQTT_FLAG_CONNECT_PASSWORD      0x40    /*!< Packet contains password */
#define MQTT_FLAG_CONNECT_WILL_RETAIN   0x20    /*!< Will retain is enabled */
#define MQTT_FLAG_CONNECT_WILL          0x04    /*!< Packet contains will topic and will message */
#define MQTT_FLAG_CONNECT_CLEAN_SESSION 0x02    /*!< Start with clean session of this client */

/* Parser states */
#define MQTT_PARSER_STATE_INIT          0x00    /*!< MQTT parser in initialized state */
#define MQTT_PARSER_STATE_CALC_REM_LEN  0x01    /*!< MQTT parser in calculating remaining length state */
#define MQTT_PARSER_STATE_READ_REM      0x02    /*!< MQTT parser in reading remaining bytes state */

/* Get packet type from incoming byte */
#define MQTT_RCV_GET_PACKET_TYPE(d)     ((mqtt_msg_type_t)(((d) >> 0x04) & 0x0F))
#define MQTT_RCV_GET_PACKET_QOS(d)      ((lwgsm_mqtt_qos_t)(((d) >> 0x01) & 0x03))
#define MQTT_RCV_GET_PACKET_DUP(d)      (((d) >> 0x03) & 0x01)

/* Requests status */
#define MQTT_REQUEST_FLAG_IN_USE        0x01    /*!< Request object is allocated and in use */
#define MQTT_REQUEST_FLAG_PENDING       0x02    /*!< Request object is pending waiting for response from server */
#define MQTT_REQUEST_FLAG_SUBSCRIBE     0x04    /*!< Request object has subscribe type */
#define MQTT_REQUEST_FLAG_UNSUBSCRIBE   0x08    /*!< Request object has unsubscribe type */

#if LWGSM_CFG_DBG

static const char*
mqtt_msg_type_to_str(mqtt_msg_type_t msg_type) {
    static const char* strings[] = {
        "UNKNOWN",
        "CONNECT", "CONNACK", "PUBLISH", "PUBACK", "PUBREC", "PUBREL",
        "PUBCOMP", "SUBSCRIBE", "SUBACK", "UNSUBSCRIBE", "UNSUBACK",
        "PINGREQ", "PINGRESP", "DISCONNECT"
    };
    return strings[(uint8_t)msg_type];
}

#endif /* LWGSM_CFG_DBG */

/**
 * \brief           Default event callback function
 * \param[in]       client: MQTT client
 * \param[in]       evt: MQTT event
 */
static void
mqtt_evt_fn_default(lwgsm_mqtt_client_p client, lwgsm_mqtt_evt_t* evt) {
    LWGSM_UNUSED(client);
    LWGSM_UNUSED(evt);
}

/**
 * \brief           Create new message ID
 * \param[in]       client: MQTT client
 * \return          New packet ID
 */
static uint16_t
create_packet_id(lwgsm_mqtt_client_p client) {
    if (++client->last_packet_id == 0) {
        client->last_packet_id = 1;
    }
    return client->last_packet_id;
}

/******************************************************************************************************/
/******************************************************************************************************/
/* MQTT requests helper function                                                                      */
/******************************************************************************************************/
/******************************************************************************************************/

/**
 * \brief           Create and return new request object
 * \param[in]       client: MQTT client
 * \param[in]       packet_id: Packet ID for QoS `1` or `2`
 * \param[in]       arg: User optional argument for identifying packets
 * \return          Pointer to new request ready to use or `NULL` if no available memory
 */
static lwgsm_mqtt_request_t*
request_create(lwgsm_mqtt_client_p client, uint16_t packet_id, void* arg) {
    lwgsm_mqtt_request_t* request;
    uint16_t i;

    /* Try to find a new request which does not have IN_USE flag set */
    for (request = NULL, i = 0; i < LWGSM_CFG_MQTT_MAX_REQUESTS; ++i) {
        if (!(client->requests[i].status & MQTT_REQUEST_FLAG_IN_USE)) {
            request = &client->requests[i];     /* We have empty request */
            break;
        }
    }
    if (request != NULL) {
        request->packet_id = packet_id;         /* Set request packet ID */
        request->arg = arg;                     /* Set user argument */
        request->status = MQTT_REQUEST_FLAG_IN_USE; /* Reset everything at this point */
    }
    return request;
}

/**
 * \brief           Delete request object and make it free
 * \param[in]       client: MQTT client
 * \param[in]       request: Request object to delete
 */
static void
request_delete(lwgsm_mqtt_client_p client, lwgsm_mqtt_request_t* request) {
    request->status = 0;                        /* Reset status to make request unused */
    LWGSM_UNUSED(client);
}

/**
 * \brief           Set request as pending waiting for server reply
 * \param[in]       client: MQTT client
 * \param[in]       request: Request object to delete
 */
static void
request_set_pending(lwgsm_mqtt_client_p client, lwgsm_mqtt_request_t* request) {
    request->timeout_start_time = lwgsm_sys_now();  /* Set timeout start time */
    request->status |= MQTT_REQUEST_FLAG_PENDING;   /* Set pending flag */
    LWGSM_UNUSED(client);
}

/**
 * \brief           Get pending request by specific packet ID
 * \param[in]       client: MQTT client
 * \param[in]       pkt_id: Packet id to get request for. Use `-1` to get first pending request
 * \return          Request on success, `NULL` otherwise
 */
static lwgsm_mqtt_request_t*
request_get_pending(lwgsm_mqtt_client_p client, int32_t pkt_id) {
    /* Try to find a new request which does not have IN_USE flag set */
    for (size_t i = 0; i < LWGSM_CFG_MQTT_MAX_REQUESTS; ++i) {
        if ((client->requests[i].status & MQTT_REQUEST_FLAG_PENDING)
            && (pkt_id == -1 || client->requests[i].packet_id == (uint16_t)pkt_id)) {
            return &client->requests[i];
        }
    }
    return NULL;
}

/**
 * \brief           Send error callback to user
 * \param[in]       client: MQTT client
 * \param[in]       status: Request status
 * \param[in]       arg: User argument
 */
static void
request_send_err_callback(lwgsm_mqtt_client_p client, uint8_t status, void* arg) {
    if (status & MQTT_REQUEST_FLAG_SUBSCRIBE) {
        client->evt.type = LWGSM_MQTT_EVT_SUBSCRIBE;
    } else if (status & MQTT_REQUEST_FLAG_UNSUBSCRIBE) {
        client->evt.type = LWGSM_MQTT_EVT_UNSUBSCRIBE;
    } else {
        client->evt.type = LWGSM_MQTT_EVT_PUBLISH;
    }

    if (client->evt.type == LWGSM_MQTT_EVT_PUBLISH) {
        client->evt.evt.publish.arg = arg;
        client->evt.evt.publish.res = lwgsmERR;
    } else {
        client->evt.evt.sub_unsub_scribed.arg = arg;
        client->evt.evt.sub_unsub_scribed.res = lwgsmERR;
    }
    client->evt_fn(client, &client->evt);
}

/******************************************************************************************************/
/******************************************************************************************************/
/* MQTT buffer helper functions                                                                       */
/******************************************************************************************************/
/******************************************************************************************************/

/**
 * \brief           Write a fixed header part of MQTT packet to output buffer
 * \param[in]       client: MQTT client
 * \param[in]       type: MQTT Message type
 * \param[in]       dup: Duplicate status when same packet is sent again
 * \param[in]       qos: Quality of service value
 * \param[in]       retain: Retain value
 * \param[in]       rem_len: Remaining packet length, excluding variable length part
 */
static void
write_fixed_header(lwgsm_mqtt_client_p client, mqtt_msg_type_t type, uint8_t dup, lwgsm_mqtt_qos_t qos, uint8_t retain, uint16_t rem_len) {
    uint8_t b;

    /*
     * Fixed header flags according to:
     * http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html#_Toc398718020
     */
    b = LWGSM_U8(type) << 0x04;
    switch (type) {
        case MQTT_MSG_TYPE_PUBLISH:
            b |= LWGSM_U8(!!dup) << 0x03 | (LWGSM_U8(qos & 0x03)) << 0x01 | LWGSM_U8(!!retain);
            break;
        case MQTT_MSG_TYPE_PUBREL:
        case MQTT_MSG_TYPE_SUBSCRIBE:
        case MQTT_MSG_TYPE_UNSUBSCRIBE:
            b |= LWGSM_U8(LWGSM_MQTT_QOS_AT_LEAST_ONCE) << 0x01;
            break;
        default:
            break;
    }
    lwgsm_buff_write(&client->tx_buff, &b, 1);  /* Write start of packet parameters */

    LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
               "[MQTT] Writing packet type %s to output buffer\r\n", mqtt_msg_type_to_str(type));

    do {                                        /* Encode length, we must write a len byte even if 0 */
        /*
         * Length is encoded LSB first up to 127 (0x7F) long,
         * where bit 7 indicates we have more data in queue for length parameter
         */
        b = LWGSM_U8((rem_len & 0x7F) | (rem_len > 0x7F ? 0x80 : 0));
        lwgsm_buff_write(&client->tx_buff, &b, 1);  /* Write single byte */
        rem_len >>= 7;                          /* Go to next 127 bytes */
    } while (rem_len > 0);
}

/**
 * \brief           Write 8-bit value to output buffer
 * \param[in]       client: MQTT client
 * \param[in]       num: Number to write
 */
static void
write_u8(lwgsm_mqtt_client_p client, uint8_t num) {
    lwgsm_buff_write(&client->tx_buff, &num, 1);/* Write single byte */
}

/**
 * \brief           Write 16-bit value in MSB first format to output buffer
 * \param[in]       client: MQTT client
 * \param[in]       num: Number to write
 */
static void
write_u16(lwgsm_mqtt_client_p client, uint16_t num) {
    write_u8(client, LWGSM_U8(num >> 8));       /* Write MSB first... */
    write_u8(client, LWGSM_U8(num & 0xFF));     /* ...followed by LSB */
}

/**
 * \brief           Write raw data without length parameter to output buffer
 * \param[in]       client: MQTT client
 * \param[in]       data: Data to write
 * \param[in]       len: Length of data to write
 */
static void
write_data(lwgsm_mqtt_client_p client, const void* data, size_t len) {
    lwgsm_buff_write(&client->tx_buff, data, len);  /* Write raw data to buffer */
}

/**
 * \brief           Check if output buffer has enough memory to handle
 *                  all bytes required to encode packet to RAW format
 *
 *                  It calculates additional bytes required to encode
 *                  remaining length itself + 1 byte for packet header
 * \param[in]       client: MQTT client
 * \param[in]       rem_len: Remaining length of packet
 * \return          Number of required RAW bytes or `0` if no memory available
 */
static uint16_t
output_check_enough_memory(lwgsm_mqtt_client_p client, uint16_t rem_len) {
    uint16_t total_len = rem_len + 1;           /* Remaining length + first (packet start) byte */

    do {                                        /* Calculate bytes for encoding remaining length itself */
        ++total_len;
        rem_len >>= 7;                          /* Encoded with 7 bits per byte */
    } while (rem_len > 0);

    return LWGSM_U16(lwgsm_buff_get_free(&client->tx_buff)) >= total_len ? total_len : 0;
}

/**
 * \brief           Write and send acknowledge/record
 * \param[in]       client: MQTT client
 * \param[in]       msg_type: Message type to respond
 * \param[in]       pkt_id: Packet ID to send response for
 * \param[in]       qos: Quality of service for packet
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
write_ack_rec_rel_resp(lwgsm_mqtt_client_p client, mqtt_msg_type_t msg_type, uint16_t pkt_id, lwgsm_mqtt_qos_t qos) {
    if (output_check_enough_memory(client, 2)) {/* Check memory for response packet */
        write_fixed_header(client, msg_type, 0, qos, 0, 2); /* Write fixed header with 2 more bytes for packet id */
        write_u16(client, pkt_id);              /* Write packet ID */
        send_data(client);                      /* Flush data to output */
        LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
                   "[MQTT] Response %s written to output memory\r\n", mqtt_msg_type_to_str(msg_type));
        return 1;
    } else {
        LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
                   "[MQTT] No memory to write %s packet\r\n", mqtt_msg_type_to_str(msg_type));
    }
    return 0;
}

/**
 * \brief           Write string to output buffer
 * \param[in]       client: MQTT client
 * \param[in]       str: String to write to buffer
 * \param[in]       len: Length of string
 */
static void
write_string(lwgsm_mqtt_client_p client, const char* str, uint16_t len) {
    write_u16(client, len);                     /* Write string length */
    lwgsm_buff_write(&client->tx_buff, str, len);   /* Write string to buffer */
}

/**
 * \brief           Send the actual data to the remote
 * \param[in]       client: MQTT client
 */
static void
send_data(lwgsm_mqtt_client_p client) {
    size_t len;
    const void* addr;

    if (client->is_sending) {                   /* We are currently sending data */
        return;
    }

    len = lwgsm_buff_get_linear_block_read_length(&client->tx_buff);/* Get length of linear memory */
    if (len > 0) {                              /* Anything to send? */
        lwgsmr_t res;
        addr = lwgsm_buff_get_linear_block_read_address(&client->tx_buff);  /* Get address of linear memory */
        if ((res = lwgsm_conn_send(client->conn, addr, len, NULL, 0)) == lwgsmOK) {
            client->written_total += len;       /* Increase number of bytes written to queue */
            client->is_sending = 1;             /* Remember active sending flag */
        } else {
            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE_WARNING,
                       "[MQTT] Cannot send data with error: %d\r\n", (int)res);
        }
    } else {
        /*
         * If buffer is empty, reset it to default state (read & write pointers)
         * This is to make sure everytime function needs to send data,
         * it can do it in single shot rather than in 2 attempts (when read > write pointer).
         * Effectively this means faster transmission of MQTT packets and lower latency.
         */
        lwgsm_buff_reset(&client->tx_buff);
    }
}

/**
 * \brief           Close a MQTT connection with server
 * \param[in]       client: MQTT client
 * \return          \ref lwgsmOK on success, member of \ref lwgsmr_t enumeration otherwise
 */
static lwgsmr_t
mqtt_close(lwgsm_mqtt_client_p client) {
    lwgsmr_t res = lwgsmERR;
    if (client->conn_state != LWGSM_MQTT_CONN_DISCONNECTED
        && client->conn_state != LWGSM_MQTT_CONN_DISCONNECTING) {

        res = lwgsm_conn_close(client->conn, 0);/* Close the connection in non-blocking mode */
        if (res == lwgsmOK) {
            client->conn_state = LWGSM_MQTT_CONN_DISCONNECTING;
        }
    }
    return res;
}

/**
 * \brief           Subscribe/Unsubscribe to/from MQTT topic
 * \param[in]       client: MQTT client
 * \param[in]       topic: MQTT topic to (un)subscribe
 * \param[in]       qos: Quality of service, used only on subscribe part
 * \param[in]       arg: Custom argument
 * \param[in]       sub: Status set to `1` on subscribe or `0` on unsubscribe
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
sub_unsub(lwgsm_mqtt_client_p client, const char* topic, lwgsm_mqtt_qos_t qos, void* arg, uint8_t sub) {
    lwgsm_mqtt_request_t* request;
    uint32_t rem_len;
    uint16_t len_topic, pkt_id;
    uint8_t ret = 0;

    if ((len_topic = LWGSM_U16(strlen(topic))) == 0) {
        return 0;
    }

    /*
     * Calculate remaining length of packet
     *
     * rem_len = 2 (topic_len) + topic_len + 2 (pkt_id) + qos (if sub)
     */
    rem_len = 2 + len_topic + 2;
    if (sub) {
        ++rem_len;
    }

    lwgsm_core_lock();
    if (client->conn_state == LWGSM_MQTT_CONNECTED
        && output_check_enough_memory(client, rem_len)) {   /* Check if enough memory to write packet data */
        pkt_id = create_packet_id(client);      /* Create new packet ID */
        request = request_create(client, pkt_id, arg);  /* Create request for packet */
        if (request != NULL) {                  /* Do we have a request */
            write_fixed_header(client, sub ? MQTT_MSG_TYPE_SUBSCRIBE : MQTT_MSG_TYPE_UNSUBSCRIBE, 0, (lwgsm_mqtt_qos_t)1, 0, rem_len);
            write_u16(client, pkt_id);          /* Write packet ID */
            write_string(client, topic, len_topic); /* Write topic string to packet */
            if (sub) {                          /* Send quality of service only on subscribe */
                write_u8(client, LWGSM_MIN(LWGSM_U8(qos), LWGSM_U8(LWGSM_MQTT_QOS_EXACTLY_ONCE)));  /* Write quality of service */
            }

            request->status |= sub ? MQTT_REQUEST_FLAG_SUBSCRIBE : MQTT_REQUEST_FLAG_UNSUBSCRIBE;
            request_set_pending(client, request);   /* Set request as pending waiting for server reply */
            send_data(client);                  /* Try to send data */
            ret = 1;
        }
    }
    lwgsm_core_unlock();
    return ret;
}

/**
 * \brief           Process incoming fully received message
 * \param[in]       client: MQTT client
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
mqtt_process_incoming_message(lwgsm_mqtt_client_p client) {
    mqtt_msg_type_t msg_type;
    lwgsm_mqtt_qos_t qos;
    uint16_t pkt_id;

    msg_type = MQTT_RCV_GET_PACKET_TYPE(client->msg_hdr_byte);  /* Get packet type from message header byte */

    /* Debug message */
    LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_STATE,
               "[MQTT] Processing packet type %s\r\n", mqtt_msg_type_to_str(msg_type));

    /* Check received packet type */
    switch (msg_type) {
        case MQTT_MSG_TYPE_CONNACK: {
            lwgsm_mqtt_conn_status_t err = (lwgsm_mqtt_conn_status_t)client->rx_buff[1];
            if (client->conn_state == LWGSM_MQTT_CONNECTING) {
                if (err == LWGSM_MQTT_CONN_STATUS_ACCEPTED) {
                    client->conn_state = LWGSM_MQTT_CONNECTED;
                }
                LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
                           "[MQTT] CONNACK received with result: %d\r\n", (int)err);

                /* Notify user layer */
                client->evt.type = LWGSM_MQTT_EVT_CONNECT;
                client->evt.evt.connect.status = err;
                client->evt_fn(client, &client->evt);
            } else {
                /* Protocol violation here */
                LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
                           "[MQTT] Protocol violation. CONNACK received when already connected!\r\n");
            }
            break;
        }
        case MQTT_MSG_TYPE_PUBLISH: {
            uint16_t topic_len, data_len;
            uint8_t* topic, *data, dup;

            qos = MQTT_RCV_GET_PACKET_QOS(client->msg_hdr_byte);/* Get QoS from received packet */
            dup = MQTT_RCV_GET_PACKET_DUP(client->msg_hdr_byte);/* Get duplicate flag */

            topic_len = (client->rx_buff[0] << 8) | client->rx_buff[1];
            topic = &client->rx_buff[2];        /* Start of topic */

            data = topic + topic_len;           /* Get data pointer */

            /* Packet ID is only available if quality of service is not 0 */
            if (qos > 0) {
                pkt_id = (client->rx_buff[2 + topic_len] << 8) | client->rx_buff[2 + topic_len + 1];/* Get packet ID */
                data += 2;                      /* Increase pointer for 2 bytes */
            } else {
                pkt_id = 0;                     /* No packet ID */
            }
            data_len = client->msg_rem_len - (data - client->rx_buff);  /* Calculate length of remaining data */

            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
                       "[MQTT] Publish packet received on topic %.*s; QoS: %d; pkt_id: %d; data_len: %d\r\n",
                       (int)topic_len, (const char*)topic, (int)qos, (int)pkt_id, (int)data_len);

            /*
             * We have to send respond to command if
             * Quality of Service is more than 0
             *
             * Response type depends on QoS and is
             * either PUBACK or PUBREC
             */
            if (qos > 0) {                      /* We have to reply on QoS > 0 */
                mqtt_msg_type_t resp_msg_type = qos == 1 ? MQTT_MSG_TYPE_PUBACK : MQTT_MSG_TYPE_PUBREC;
                LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE, "[MQTT] Sending publish resp: %s on pkt_id: %d\r\n", \
                           mqtt_msg_type_to_str(resp_msg_type), (int)pkt_id);

                write_ack_rec_rel_resp(client, resp_msg_type, pkt_id, qos);
            }

            /* Notify application layer about received packet */
            client->evt.type = LWGSM_MQTT_EVT_PUBLISH_RECV;
            client->evt.evt.publish_recv.topic = topic;
            client->evt.evt.publish_recv.topic_len = topic_len;
            client->evt.evt.publish_recv.payload = data;
            client->evt.evt.publish_recv.payload_len = data_len;
            client->evt.evt.publish_recv.dup = dup;
            client->evt.evt.publish_recv.qos = qos;
            client->evt_fn(client, &client->evt);
            break;
        }
        case MQTT_MSG_TYPE_PINGRESP: {          /* Respond to PINGREQ received */
            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE, "[MQTT] Ping response received\r\n");

            client->evt.type = LWGSM_MQTT_EVT_KEEP_ALIVE;
            client->evt_fn(client, &client->evt);
            break;
        }
        case MQTT_MSG_TYPE_SUBACK:
        case MQTT_MSG_TYPE_UNSUBACK:
        case MQTT_MSG_TYPE_PUBREC:
        case MQTT_MSG_TYPE_PUBREL:
        case MQTT_MSG_TYPE_PUBACK:
        case MQTT_MSG_TYPE_PUBCOMP: {
            pkt_id = client->rx_buff[0] << 8 | client->rx_buff[1];  /* Get packet ID */

            if (msg_type == MQTT_MSG_TYPE_PUBREC) { /* Publish record received from server */
                write_ack_rec_rel_resp(client, MQTT_MSG_TYPE_PUBREL, pkt_id, (lwgsm_mqtt_qos_t)1);  /* Send back publish release message */
            } else if (msg_type == MQTT_MSG_TYPE_PUBREL) {  /* Publish release was received */
                write_ack_rec_rel_resp(client, MQTT_MSG_TYPE_PUBCOMP, pkt_id, (lwgsm_mqtt_qos_t)0); /* Send back publish complete */
            } else if (msg_type == MQTT_MSG_TYPE_SUBACK
                       || msg_type == MQTT_MSG_TYPE_UNSUBACK
                       || msg_type == MQTT_MSG_TYPE_PUBACK
                       || msg_type == MQTT_MSG_TYPE_PUBCOMP) {
                lwgsm_mqtt_request_t* request;

                /*
                 * We can enter here only if we received final acknowledge
                 * on request packets we sent first.
                 *
                 * At these point we should have a pending request
                 * waiting for final acknowledge, otherwise there is protocol violation
                 */
                request = request_get_pending(client, pkt_id);  /* Get pending request by packet ID */
                if (request != NULL) {
                    if (msg_type == MQTT_MSG_TYPE_SUBACK
                        || msg_type == MQTT_MSG_TYPE_UNSUBACK) {
                        client->evt.type = msg_type == MQTT_MSG_TYPE_SUBACK ? LWGSM_MQTT_EVT_SUBSCRIBE : LWGSM_MQTT_EVT_UNSUBSCRIBE;
                        client->evt.evt.sub_unsub_scribed.arg = request->arg;
                        client->evt.evt.sub_unsub_scribed.res = client->rx_buff[2] < 3 ? lwgsmOK : lwgsmERR;
                        client->evt_fn(client, &client->evt);

                        /*
                         * Final acknowledge of packet received
                         * Ack type depends on QoS level being sent to server on request
                         */
                    } else if (msg_type == MQTT_MSG_TYPE_PUBCOMP
                               || msg_type == MQTT_MSG_TYPE_PUBACK) {
                        client->evt.type = LWGSM_MQTT_EVT_PUBLISH;
                        client->evt.evt.publish.arg = request->arg;
                        client->evt.evt.publish.res = lwgsmOK;
                        client->evt_fn(client, &client->evt);
                    }
                    request_delete(client, request);/* Delete request object */
                } else {
                    /* Protocol violation at this point! */
                    LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
                               "[MQTT] Protocol violation. Received ACK without sent packet\r\n");
                }
            }
            break;
        }
        default:
            return 0;
    }
    return 1;
}

/**
 * \brief           Parse incoming buffer data and try to construct clean packet from it
 * \param[in]       client: MQTT client
 * \param[in]       pbuf: Received packet buffer with data
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
mqtt_parse_incoming(lwgsm_mqtt_client_p client, lwgsm_pbuf_p pbuf) {
    size_t buff_len = 0, buff_offset = 0;
    uint8_t ch, *d;

    do {
        buff_offset += buff_len;                /* Calculate new offset of buffer */
        d = lwgsm_pbuf_get_linear_addr(pbuf, buff_offset, &buff_len);   /* Get address pointer */
        if (d == NULL) {
            break;
        }
        for (size_t idx = 0; idx < buff_len; ++idx) {   /* Process entire linear buffer */
            ch = d[idx];
            switch (client->parser_state) {     /* Check parser state */
                case MQTT_PARSER_STATE_INIT: {  /* We are waiting for start byte and packet type */
                    LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_STATE,
                               "[MQTT] Parser init state, received first byte of packet 0x%02X\r\n", (unsigned)ch);

                    /* Save other info about message */
                    client->msg_hdr_byte = ch;  /* Save first entry */
                    client->msg_rem_len = 0;    /* Reset remaining length */
                    client->msg_rem_len_mult = 0;   /* Reset length multiplier */
                    client->msg_curr_pos = 0;   /* Reset current buffer write pointer */

                    client->parser_state = MQTT_PARSER_STATE_CALC_REM_LEN;
                    break;
                }
                case MQTT_PARSER_STATE_CALC_REM_LEN: {  /* Calculate remaining length of packet */
                    /* Length of packet is LSB first, each consist of up to 7 bits */
                    client->msg_rem_len |= (ch & 0x7F) << ((size_t)7 * (size_t)client->msg_rem_len_mult);
                    ++client->msg_rem_len_mult;

                    if (!(ch & 0x80)) {         /* Is this last entry? */
                        LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_STATE,
                                   "[MQTT] Remaining length received: %d bytes\r\n", (int)client->msg_rem_len);

                        if (client->msg_rem_len > 0) {
                            /*
                             * Check if all data bytes are part of single pbuf.
                             * this is done by check if current idx position vs length is more than expected data length
                             * Check must be "greater as" due to idx currently pointing to last length byte and not beginning of data
                             */
                            if ((buff_len - idx) > client->msg_rem_len) {
                                void* tmp_ptr = client->rx_buff;
                                size_t tmp_len = client->rx_buff_len;

                                /* Set new client pointer */
                                client->rx_buff = &d[idx + 1];  /* Data are one byte after */
                                client->rx_buff_len = client->msg_rem_len;

                                mqtt_process_incoming_message(client);  /* Process new message */

                                /* Reset to previous values */
                                client->rx_buff = tmp_ptr;
                                client->rx_buff_len = tmp_len;
                                client->parser_state = MQTT_PARSER_STATE_INIT;

                                idx += client->msg_rem_len; /* Skip data part only, idx is increased again in for loop */
                            } else {
                                client->parser_state = MQTT_PARSER_STATE_READ_REM;
                            }
                        } else {
                            mqtt_process_incoming_message(client);
                            client->parser_state = MQTT_PARSER_STATE_INIT;
                        }
                    }
                    break;
                }
                case MQTT_PARSER_STATE_READ_REM: {  /* Read remaining bytes and write to RX buffer */
                    /* Process only if rx buff length is big enough */
                    if (client->msg_curr_pos < client->rx_buff_len) {
                        client->rx_buff[client->msg_curr_pos] = ch; /* Write received character */
                    }
                    ++client->msg_curr_pos;

                    /* We reached end of received characters? */
                    if (client->msg_curr_pos == client->msg_rem_len) {
                        if (client->msg_curr_pos <= client->rx_buff_len) {  /* Check if it was possible to write all data to rx buffer */
                            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_STATE,
                                       "[MQTT] Packet parsed and ready for processing\r\n");

                            mqtt_process_incoming_message(client);  /* Process incoming packet */
                        } else {
                            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE_WARNING,
                                       "[MQTT] Packet too big for rx buffer. Packet discarded\r\n");
                        }
                        client->parser_state = MQTT_PARSER_STATE_INIT;  /* Go to initial state and listen for next received packet */
                    }
                    break;
                }
                default:
                    client->parser_state = MQTT_PARSER_STATE_INIT;
            }
        }
    } while (buff_len > 0);
    return 0;
}

/******************************************************************************************************/
/******************************************************************************************************/
/* Connection callback functions                                                                      */
/******************************************************************************************************/
/******************************************************************************************************/

/**
 * \brief           Callback when we are connected to MQTT server
 * \param[in]       client: MQTT client
 */
static void
mqtt_connected_cb(lwgsm_mqtt_client_p client) {
    uint16_t rem_len, len_id, len_pass = 0, len_user = 0, len_will_topic = 0, len_will_message = 0;
    uint8_t flags = 0;

    flags |= MQTT_FLAG_CONNECT_CLEAN_SESSION;   /* Start as clean session */

    /*
     * Remaining length consist of fixed header data
     * variable header and possible data
     *
     * Minimum length consists of 2 + "MQTT" (4) + protocol_level (1) + flags (1) + keep_alive (2)
     */
    rem_len = 10;                               /* Set remaining length of fixed header */

    len_id = LWGSM_U16(strlen(client->info->id));   /* Get cliend ID length */
    rem_len += len_id + 2;                      /* Add client id length including length entries */

    if (client->info->will_topic != NULL && client->info->will_message != NULL) {
        flags |= MQTT_FLAG_CONNECT_WILL;
        flags |= LWGSM_MIN(LWGSM_U8(client->info->will_qos), 2) << 0x03;/* Set qos to flags */

        len_will_topic = LWGSM_U16(strlen(client->info->will_topic));
        len_will_message = LWGSM_U16(strlen(client->info->will_message));

        rem_len += len_will_topic + 2;          /* Add will topic parameter */
        rem_len += len_will_message + 2;        /* Add will message parameter */
    }

    if (client->info->user != NULL) {           /* Check for username */
        flags |= MQTT_FLAG_CONNECT_USERNAME;    /* Username is included */

        len_user = LWGSM_U16(strlen(client->info->user));   /* Get username length */
        rem_len += len_user + 2;                /* Add username length including length entries */
    }

    if (client->info->pass != NULL) {           /* Check for password */
        flags |= MQTT_FLAG_CONNECT_PASSWORD;    /* Password is included */

        len_pass = LWGSM_U16(strlen(client->info->pass));   /* Get username length */
        rem_len += len_pass + 2;                /* Add password length including length entries */
    }

    if (!output_check_enough_memory(client, rem_len)) { /* Is there enough memory to write everything? */
        return;
    }

    /* Write everything to output buffer */
    write_fixed_header(client, MQTT_MSG_TYPE_CONNECT, 0, (lwgsm_mqtt_qos_t)0, 0, rem_len);
    write_string(client, "MQTT", 4);            /* Protocol name */
    write_u8(client, 4);                        /* Protocol version */
    write_u8(client, flags);                    /* Flags for CONNECT message */
    write_u16(client, client->info->keep_alive);/* Keep alive timeout in units of seconds */
    write_string(client, client->info->id, len_id); /* This is client ID string */
    if (flags & MQTT_FLAG_CONNECT_WILL) {       /* Check for will topic */
        write_string(client, client->info->will_topic, len_will_topic); /* Write topic to packet */
        write_string(client, client->info->will_message, len_will_message); /* Write message to packet */
    }
    if (flags & MQTT_FLAG_CONNECT_USERNAME) {   /* Check for username */
        write_string(client, client->info->user, len_user); /* Write username to packet */
    }
    if (flags & MQTT_FLAG_CONNECT_PASSWORD) {   /* Check for password */
        write_string(client, client->info->pass, len_pass); /* Write password to packet */
    }

    client->parser_state = MQTT_PARSER_STATE_INIT;  /* Reset parser state */

    client->poll_time = 0;                      /* Reset kep alive time */
    client->conn_state = LWGSM_MQTT_CONNECTING; /* MQTT is connecting to server */

    send_data(client);                          /* Flush and send the actual data */
}

/**
 * \brief           Received data callback function
 * \param[in]       client: MQTT client
 * \param[in]       pbuf: Received packet buffer with data
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
mqtt_data_recv_cb(lwgsm_mqtt_client_p client, lwgsm_pbuf_p pbuf) {
    mqtt_parse_incoming(client, pbuf);          /* We need to process incoming data */
    lwgsm_conn_recved(client->conn, pbuf);      /* Notify stack about received data */
    return 1;
}

/**
 * \brief           Data sent callback
 * \param[in]       client: MQTT client
 * \param[in]       sent_len: Number of bytes sent (or not)
 * \param[in]       successful: Send status. Set to `1` on success or `0` if send error occurred
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
mqtt_data_sent_cb(lwgsm_mqtt_client_p client, size_t sent_len, uint8_t successful) {
    lwgsm_mqtt_request_t* request;

    client->is_sending = 0;                     /* We are not sending anymore */
    client->sent_total += sent_len;

    client->poll_time = 0;                      /* Reset kep alive time */

    /*
     * In case transmit was not successful,
     * start procedure to close MQTT connection
     * and clear all pending requests in closed callback function
     */
    if (!successful) {
        LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE_WARNING,
                   "[MQTT] Failed to send %d bytes. Manually closing down..\r\n", (int)sent_len);

        mqtt_close(client);
        return 0;
    }
    lwgsm_buff_skip(&client->tx_buff, sent_len);/* Skip buffer for actual sent data */

    /*
     * Check pending publish requests without QoS because there is no confirmation received by server.
     * Use technique to count number of bytes sent versus expected number of bytes sent before we ack request sent
     *
     * Requests without QoS have packet id set to 0
     */
    while ((request = request_get_pending(client, 0)) != NULL) {
        if (client->sent_total >= request->expected_sent_len) {
            void* arg = request->arg;

            request_delete(client, request);    /* Delete request and make space for next command */

            /* Call published callback */
            client->evt.type = LWGSM_MQTT_EVT_PUBLISH;
            client->evt.evt.publish.arg = arg;
            client->evt.evt.publish.res = lwgsmOK;
            client->evt_fn(client, &client->evt);
        } else {
            break;
        }
    }

    send_data(client);                          /* Try to send more */
    return 1;
}

/**
 * \brief           Poll for client connection
 *                  Called every LWGSM_CFG_CONN_POLL_INTERVAL ms when MQTT client TCP connection is established
 * \param[in]       client: MQTT client
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
mqtt_poll_cb(lwgsm_mqtt_client_p client) {
    ++client->poll_time;

    if (client->conn_state == LWGSM_MQTT_CONN_DISCONNECTING) {
        return 0;
    }

    /*
     * Check for keep-alive time if equal or greater than
     * keep alive time. In that case, send packet
     * to make sure we are still alive
     */
    if (client->info->keep_alive                /* Keep alive must be enabled */
        /* Poll time is in units of LWGSM_CFG_CONN_POLL_INTERVAL milliseconds,
           while keep_alive is in units of seconds */
        && (client->poll_time * LWGSM_CFG_CONN_POLL_INTERVAL) >= (uint32_t)(client->info->keep_alive * 1000)) {

        if (output_check_enough_memory(client, 0)) {/* Check if memory available in output buffer */
            write_fixed_header(client, MQTT_MSG_TYPE_PINGREQ, 0, (lwgsm_mqtt_qos_t)0, 0, 0);/* Write PINGREQ command to output buffer */
            send_data(client);                  /* Force send data */
            client->poll_time = 0;              /* Reset polling time */

            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE, "[MQTT] Sending PINGREQ packet\r\n");
        } else {
            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE_WARNING, "[MQTT] No memory to send PINGREQ packet\r\n");
        }
    }

    /*
     * Process all active packets and
     * check for timeout if there was no reply from MQTT server
     */
    return 1;
}

/**
 * \brief           Connection closed callback
 * \param[in]       client: MQTT client
 * \param[in]       res: Result of close event
 * \param[in]       forced: Set to `1` when closed by user
 * \return          `1` on success, `0` otherwise
 */
static uint8_t
mqtt_closed_cb(lwgsm_mqtt_client_p client, lwgsmr_t res, uint8_t forced) {
    lwgsm_mqtt_state_t state = client->conn_state;
    lwgsm_mqtt_request_t* request;

    /*
     * Call user function only if connection was closed
     * when we are connected or in disconnecting mode
     */
    client->conn_state = LWGSM_MQTT_CONN_DISCONNECTED;  /* Connection is disconnected, ready to be established again */
    client->evt.evt.disconnect.is_accepted = state == LWGSM_MQTT_CONNECTED || state == LWGSM_MQTT_CONN_DISCONNECTING;   /* Set connection state */
    client->evt.type = LWGSM_MQTT_EVT_DISCONNECT;   /* Connection disconnected from server */
    client->evt_fn(client, &client->evt);       /* Notify upper layer about closed connection */
    client->conn = NULL;                        /* Reset connection handle */

    /* Check all requests */
    while ((request = request_get_pending(client, -1)) != NULL) {
        uint8_t status = request->status;
        void* arg = request->arg;

        request_delete(client, request);        /* Delete request */
        request_send_err_callback(client, status, arg); /* Send error callback to user */
    }
    LWGSM_MEMSET(client->requests, 0x00, sizeof(client->requests));

    client->is_sending = client->sent_total = client->written_total = 0;
    client->parser_state = MQTT_PARSER_STATE_INIT;
    lwgsm_buff_reset(&client->tx_buff);         /* Reset TX buffer */

    LWGSM_UNUSED(forced);

    return 1;
}

/**
 * \brief           Connection callback
 * \param[in]       evt: Callback parameters
 * \result          \ref lwgsmOK on success, member of \ref lwgsmr_t enumeration otherwise
 */
static lwgsmr_t
mqtt_conn_cb(lwgsm_evt_t* evt) {
    lwgsm_conn_p conn;
    lwgsm_mqtt_client_p client = NULL;

    conn = lwgsm_conn_get_from_evt(evt);        /* Get connection from event */
    if (conn != NULL) {
        client = lwgsm_conn_get_arg(conn);      /* Get client structure from connection */
        if (client == NULL) {
            lwgsm_conn_close(conn, 0);          /* Force connection close immediately */
            return lwgsmERR;
        }
    } else if (evt->type != LWGSM_EVT_CONN_ERROR) {
        return lwgsmERR;
    }

    /* Check and process events */
    switch (lwgsm_evt_get_type(evt)) {
        /*
         * Connection error. Connection to external
         * server was not successful
         */
        case LWGSM_EVT_CONN_ERROR: {
            lwgsm_mqtt_client_p client;
            client = lwgsm_evt_conn_error_get_arg(evt); /* Get connection argument */
            if (client != NULL) {
                client->conn_state = LWGSM_MQTT_CONN_DISCONNECTED;  /* Set back to disconnected state */
                /* Notify user upper layer */
                client->evt.type = LWGSM_MQTT_EVT_CONNECT;
                client->evt.evt.connect.status = LWGSM_MQTT_CONN_STATUS_TCP_FAILED; /* TCP connection failed */
                client->evt_fn(client, &client->evt);   /* Notify upper layer about closed connection */
            }
            break;
        }

        /* Connection active to MQTT server */
        case LWGSM_EVT_CONN_ACTIVE: {
            mqtt_connected_cb(client);          /* Call function to process status */
            break;
        }

        /* A new packet of data received on MQTT client connection */
        case LWGSM_EVT_CONN_RECV: {
            mqtt_data_recv_cb(client, lwgsm_evt_conn_recv_get_buff(evt));   /* Call user to process received data */
            break;
        }

        /* Data send event */
        case LWGSM_EVT_CONN_SEND: {
            /* Data sent callback */
            mqtt_data_sent_cb(client,
                              lwgsm_evt_conn_send_get_length(evt),
                              lwgsm_evt_conn_send_get_result(evt) == lwgsmOK);
            break;
        }

        /* Periodic poll for connection */
        case LWGSM_EVT_CONN_POLL: {
            mqtt_poll_cb(client);               /* Poll client */
            break;
        }

        /* Connection closed */
        case LWGSM_EVT_CONN_CLOSE: {
            mqtt_closed_cb(client,
                           lwgsm_evt_conn_close_get_result(evt) == lwgsmOK,
                           lwgsm_evt_conn_close_is_forced(evt));
            break;
        }
        default:
            break;
    }
    return lwgsmOK;
}

/**
 * \brief           Allocate a new MQTT client structure
 * \param[in]       tx_buff_len: Length of raw data output buffer
 * \param[in]       rx_buff_len: Length of raw data input buffer
 * \return          Pointer to new allocated MQTT client structure or `NULL` on failure
 */
lwgsm_mqtt_client_t*
lwgsm_mqtt_client_new(size_t tx_buff_len, size_t rx_buff_len) {
    lwgsm_mqtt_client_p client;

    client = lwgsm_mem_malloc(sizeof(*client));
    if (client != NULL) {
        LWGSM_MEMSET(client, 0x00, sizeof(*client));
        client->conn_state = LWGSM_MQTT_CONN_DISCONNECTED;  /* Set to disconnected mode */

        if (!lwgsm_buff_init(&client->tx_buff, tx_buff_len)) {
            lwgsm_mem_free_s((void**)&client);
        }
        if (client != NULL) {
            client->rx_buff_len = rx_buff_len;
            client->rx_buff = lwgsm_mem_malloc(rx_buff_len);
            if (client->rx_buff == NULL) {
                lwgsm_buff_free(&client->tx_buff);
                lwgsm_mem_free_s((void**)&client);
            }
        }
    }
    return client;
}

/**
 * \brief           Delete MQTT client structure
 * \note            MQTT client must be disconnected first
 * \param[in]       client: MQTT client
 */
void
lwgsm_mqtt_client_delete(lwgsm_mqtt_client_p client) {
    if (client != NULL) {
        lwgsm_mem_free_s((void**)&client->rx_buff);
        lwgsm_buff_free(&client->tx_buff);
        lwgsm_mem_free_s((void**)&client);
    }
}

/**
 * \brief           Connect to MQTT server
 * \note            After TCP connection is established, CONNECT packet is automatically sent to server
 * \param[in]       client: MQTT client
 * \param[in]       host: Host address for server
 * \param[in]       port: Host port number
 * \param[in]       evt_fn: Callback function for all events on this MQTT client
 * \param[in]       info: Information structure for connection
 * \return          \ref lwgsmOK on success, member of \ref lwgsmr_t enumeration otherwise
 */
lwgsmr_t
lwgsm_mqtt_client_connect(lwgsm_mqtt_client_p client, const char* host, lwgsm_port_t port,
                        lwgsm_mqtt_evt_fn evt_fn, const lwgsm_mqtt_client_info_t* info) {
    lwgsmr_t res = lwgsmERR;

    LWGSM_ASSERT("client != NULL", client != NULL); /* t input parameters */
    LWGSM_ASSERT("host != NULL", host != NULL);
    LWGSM_ASSERT("port > 0", port > 0);
    LWGSM_ASSERT("info != NULL", info != NULL);

    lwgsm_core_lock();
    if (lwgsm_network_is_attached() && client->conn_state == LWGSM_MQTT_CONN_DISCONNECTED) {
        client->info = info;                    /* Save client info parameters */
        client->evt_fn = evt_fn != NULL ? evt_fn : mqtt_evt_fn_default;

        /* Start a new connection in non-blocking mode */
        res = lwgsm_conn_start(&client->conn, LWGSM_CONN_TYPE_TCP, host, port, client, mqtt_conn_cb, 0);
        if (res == lwgsmOK) {
            client->conn_state = LWGSM_MQTT_CONN_CONNECTING;
        }
    }
    lwgsm_core_unlock();

    return res;
}

/**
 * \brief           Disconnect from MQTT server
 * \param[in]       client: MQTT client
 * \return          \ref lwgsmOK if request sent to queue or member of \ref lwgsmr_t otherwise
 */
lwgsmr_t
lwgsm_mqtt_client_disconnect(lwgsm_mqtt_client_p client) {
    lwgsmr_t res = lwgsmERR;

    lwgsm_core_lock();
    if (client->conn_state != LWGSM_MQTT_CONN_DISCONNECTED
        && client->conn_state != LWGSM_MQTT_CONN_DISCONNECTING) {
        res = mqtt_close(client);               /* Close client connection */
    }
    lwgsm_core_unlock();
    return res;
}

/**
 * \brief           Subscribe to MQTT topic
 * \param[in]       client: MQTT client
 * \param[in]       topic: Topic name to subscribe to
 * \param[in]       qos: Quality of service. This parameter can be a value of \ref lwgsm_mqtt_qos_t
 * \param[in]       arg: User custom argument used in callback
 * \return          \ref lwgsmOK on success, member of \ref lwgsmr_t enumeration otherwise
 */
lwgsmr_t
lwgsm_mqtt_client_subscribe(lwgsm_mqtt_client_p client, const char* topic, lwgsm_mqtt_qos_t qos, void* arg) {
    return sub_unsub(client, topic, qos, arg, 1) == 1 ? lwgsmOK : lwgsmERR; /* Subscribe to topic */
}

/**
 * \brief           Unsubscribe from MQTT topic
 * \param[in]       client: MQTT client
 * \param[in]       topic: Topic name to unsubscribe from
 * \param[in]       arg: User custom argument used in callback
 * \return          \ref lwgsmOK on success, member of \ref lwgsmr_t enumeration otherwise
 */
lwgsmr_t
lwgsm_mqtt_client_unsubscribe(lwgsm_mqtt_client_p client, const char* topic, void* arg) {
    return sub_unsub(client, topic, (lwgsm_mqtt_qos_t)0, arg, 0) == 1 ? lwgsmOK : lwgsmERR; /* Unsubscribe from topic */
}

/**
 * \brief           Publish a new message on specific topic
 * \param[in]       client: MQTT client
 * \param[in]       topic: Topic to send message to
 * \param[in]       payload: Message data
 * \param[in]       payload_len: Length of payload data
 * \param[in]       qos: Quality of service. This parameter can be a value of \ref lwgsm_mqtt_qos_t enumeration
 * \param[in]       retain: Retian parameter value
 * \param[in]       arg: User custom argument used in callback
 * \return          \ref lwgsmOK on success, member of \ref lwgsmr_t enumeration otherwise
 */
lwgsmr_t
lwgsm_mqtt_client_publish(lwgsm_mqtt_client_p client, const char* topic, const void* payload,
                        uint16_t payload_len, lwgsm_mqtt_qos_t qos, uint8_t retain, void* arg) {
    lwgsmr_t res = lwgsmOK;
    lwgsm_mqtt_request_t* request = NULL;
    uint32_t rem_len, raw_len;
    uint16_t len_topic, pkt_id;
    uint8_t qos_u8 = LWGSM_U8(qos);

    if (!(len_topic = LWGSM_U16(strlen(topic)))) {  /* Get length of topic */
        return lwgsmERR;
    }

    /*
     * Calculate remaining length of packet
     *
     * rem_len = 2 (topic_len) + topic_len + 2 (pkt_idm only if qos > 0) + payload_len
     */
    rem_len = 2 + len_topic + (payload != NULL ? payload_len : 0);
    if (qos_u8 > 0) {
        rem_len += 2;
    }

    lwgsm_core_lock();
    if (client->conn_state != LWGSM_MQTT_CONNECTED) {
        res = lwgsmCLOSED;
    } else if ((raw_len = output_check_enough_memory(client, rem_len)) != 0) {
        pkt_id = qos_u8 > 0 ? create_packet_id(client) : 0; /* Create new packet ID */
        request = request_create(client, pkt_id, arg);  /* Create request for packet */
        if (request != NULL) {
            /*
             * Set expected number of bytes we should send before
             * we can say that this packet was sent.
             * Used in case QoS is set to 0 where packet notification
             * is not received by server. In this case, wait
             * number of bytes sent before notifying user about success
             */
            request->expected_sent_len = client->written_total + raw_len;

            write_fixed_header(client, MQTT_MSG_TYPE_PUBLISH, 0, (lwgsm_mqtt_qos_t)LWGSM_MIN(qos_u8, LWGSM_U8(LWGSM_MQTT_QOS_EXACTLY_ONCE)), retain, rem_len);
            write_string(client, topic, len_topic); /* Write topic string to packet */
            if (qos_u8) {
                write_u16(client, pkt_id);      /* Write packet ID */
            }
            if (payload != NULL && payload_len) {
                write_data(client, payload, payload_len);   /* Write RAW topic payload */
            }
            request_set_pending(client, request);   /* Set request as pending waiting for server reply */

            send_data(client);                  /* Try to send data */

            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE,
                       "[MQTT] Pkt publish start. QoS: %d, pkt_id: %d\r\n", (int)qos_u8, (int)pkt_id);
        } else {
            LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE, "[MQTT] No free request available to publish message\r\n");
            res = lwgsmERRMEM;
        }
    } else {
        LWGSM_DEBUGF(LWGSM_CFG_DBG_MQTT_TRACE, "[MQTT] Not enough memory to publish message\r\n");
        res = lwgsmERRMEM;
    }
    lwgsm_core_unlock();
    return res;
}

/**
 * \brief           Test if client is connected to server and accepted to MQTT protocol
 * \note            Function will return error if TCP is connected but MQTT not accepted
 * \param[in]       client: MQTT client
 * \return          `1` on success, `0` otherwise
 */
uint8_t
lwgsm_mqtt_client_is_connected(lwgsm_mqtt_client_p client) {
    uint8_t res;

    lwgsm_core_lock();
    res = LWGSM_U8(client->conn_state == LWGSM_MQTT_CONNECTED);
    lwgsm_core_unlock();

    return res;
}

/**
 * \brief           Set user argument on client
 * \param[in]       client: MQTT client handle
 * \param[in]       arg: User argument
 */
void
lwgsm_mqtt_client_set_arg(lwgsm_mqtt_client_p client, void* arg) {
    lwgsm_core_lock();
    client->arg = arg;
    lwgsm_core_unlock();
}

/**
 * \brief           Get user argument on client
 * \param[in]       client: MQTT client handle
 * \return          User argument
 */
void*
lwgsm_mqtt_client_get_arg(lwgsm_mqtt_client_p client) {
    return client->arg;
}
