/**
 * \file            lwgsm_opts.h
 * \brief           GSM application options
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
 * This file is part of LwGSM.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 * Version:         v0.1.0
 */
#ifndef LWGSM_HDR_OPTS_H
#define LWGSM_HDR_OPTS_H

/* Rename this file to "lwgsm_opts.h" for your application */

/*
 * Open "include/lwgsm/lwgsm_opt.h" and
 * copy & replace here settings you want to change values
 */
#if !__DOXYGEN__
#define LWGSM_CFG_DBG                         LWGSM_DBG_ON
#define LWGSM_CFG_DBG_TYPES_ON                LWGSM_DBG_TYPE_TRACE | LWGSM_DBG_TYPE_STATE
#define LWGSM_CFG_DBG_MQTT                    LWGSM_DBG_OFF

#define LWGSM_CFG_IPD_MAX_BUFF_SIZE           1460
#define LWGSM_CFG_INPUT_USE_PROCESS           1
#define LWGSM_CFG_AT_ECHO                     0

#define LWGSM_CFG_NETWORK                     1

#define LWGSM_CFG_CONN                        1
#define LWGSM_CFG_SMS                         1
#define LWGSM_CFG_CALL                        1
#define LWGSM_CFG_PHONEBOOK                   1
#define LWGSM_CFG_USSD                        1

#define LWGSM_CFG_USE_API_FUNC_EVT            1

#define LWGSM_CFG_NETCONN                     1

#define LWGSM_CFG_MEM_CUSTOM                  1

#if defined(WIN32)
#define LWGSM_CFG_SYS_PORT					LWGSM_SYS_PORT_WIN32
#endif

#endif /* !__DOXYGEN__ */

#endif /* LWGSM_HDR_OPTS_H */
