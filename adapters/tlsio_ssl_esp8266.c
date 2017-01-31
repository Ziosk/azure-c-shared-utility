// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#ifdef FREERTOS_ARCH_ESP8266
#include "openssl/ssl_compat-1.0.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "espressif/esp8266/ets_sys.h"
#include "espressif/espconn.h"

#else
//mock header
#include "esp8266_mock.h"
#include "azure_c_shared_utility/gballoc.h"
#endif

#include <stdio.h>
#include <stdbool.h>
#include "azure_c_shared_utility/lock.h"
/* Codes_SRS_TLSIO_SSL_ESP8266_99_001: [ The tlsio_ssl_esp8266 shall implement and export all the Concrete functions in the VTable IO_INTERFACE_DESCRIPTION defined in the `xio.h`. ]*/
/* Codes_SRS_TLSIO_SSL_ESP8266_99_002: [ The tlsio_ssl_esp8266 shall report the open operation status using the IO_OPEN_RESULT enumerator defined in the `xio.h`.]*/
/* Codes_SRS_TLSIO_SSL_ESP8266_99_003: [ The tlsio_ssl_esp8266 shall report the send operation status using the IO_SEND_RESULT enumerator defined in the `xio.h`. ]*/
/* Codes_SRS_TLSIO_SSL_ESP8266_99_004: [ The tlsio_ssl_esp8266 shall call the callbacks functions defined in the `xio.h`. ]*/
#include "azure_c_shared_utility/tlsio_openssl.h"
/* Codes_SRS_TLSIO_SSL_ESP8266_99_005: [ The tlsio_ssl_esp8266 shall received the connection information using the TLSIO_CONFIG structure defined in `tlsio.h`. ]*/
#include "azure_c_shared_utility/tlsio.h"
#include "azure_c_shared_utility/socketio.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/crt_abstractions.h"

#define OPENSSL_FRAGMENT_SIZE 5120
#define OPENSSL_LOCAL_TCP_PORT 1000
#define MAX_RETRY 20
#define MAX_RETRY_WRITE 500
#define RETRY_DELAY 1000

typedef enum TLSIO_STATE_TAG
{
    TLSIO_STATE_NOT_OPEN,
    TLSIO_STATE_OPENING,
    TLSIO_STATE_OPEN,
    TLSIO_STATE_CLOSING,
    TLSIO_STATE_ERROR
} TLSIO_STATE;

typedef struct TLS_IO_INSTANCE_TAG
{
    ON_BYTES_RECEIVED on_bytes_received;
    ON_IO_OPEN_COMPLETE on_io_open_complete;
    ON_IO_CLOSE_COMPLETE on_io_close_complete;
    ON_IO_ERROR on_io_error;
    void* on_bytes_received_context;
    void* on_io_open_complete_context;
    void* on_io_close_complete_context;
    void* on_io_error_context;
    SSL* ssl;
    SSL_CTX* ssl_context;
    TLSIO_STATE tlsio_state;
    char* hostname;
    int port;
    char* certificate;
    const char* x509certificate;
    const char* x509privatekey;
    int sock;
} TLS_IO_INSTANCE;


int ICACHE_FLASH_ATTR ERR_get_error(void)
{
    return 0;
}

void ICACHE_FLASH_ATTR ERR_error_string_n(uint32 error, char* out, uint32 olen)
{
    return;
}

void ICACHE_FLASH_ATTR ERR_error_string(uint32 error, char* out)
{
    return;
}

/*this function destroys an option previously created*/
static void tlsio_openssl_DestroyOption(const char* name, const void* value)
{
    /*since all options for this layer are actually string copies, disposing of one is just calling free*/
    if (
        (name == NULL) || (value == NULL)
        )
    {
        LogError("invalid parameter detected: const char* name=%p, const void* value=%p", name, value);
    }
    else
    {
        if (
            (strcmp(name, "TrustedCerts") == 0) ||
            (strcmp(name, "x509certificate") == 0) ||
            (strcmp(name, "x509privatekey") == 0)
        )
        {
            free((void*)value);
        }
        else
        {
            LogError("not handled option : %s", name);
        }
    }
}


static OPTIONHANDLER_HANDLE tlsio_openssl_retrieveoptions(CONCRETE_IO_HANDLE handle)
{
    OPTIONHANDLER_HANDLE result = NULL;
    return result;
}

static const IO_INTERFACE_DESCRIPTION tlsio_openssl_interface_description =
{
    tlsio_openssl_retrieveoptions,
    tlsio_openssl_create,
    tlsio_openssl_destroy,
    tlsio_openssl_open,
    tlsio_openssl_close,
    tlsio_openssl_send,
    tlsio_openssl_dowork,
    tlsio_openssl_setoption
};

static LOCK_HANDLE * openssl_locks = NULL;

static void openssl_dynamic_locks_uninstall(void)
{
}

static void openssl_dynamic_locks_install(void)
{
}

static void openssl_static_locks_lock_unlock_cb(int lock_mode, int lock_index, const char * file, int line)
{
}

static void indicate_open_complete(TLS_IO_INSTANCE* tls_io_instance, IO_OPEN_RESULT open_result)
{
    if (tls_io_instance->on_io_open_complete == NULL)
    {
        LogError("NULL on_io_open_complete.");
    }
    else
    {
        /* Codes_TLSIO_SSL_ESP8266_99_002: [ The tlsio_ssl_esp8266 shall report the open operation status using the IO_OPEN_RESULT enumerator defined in the `xio.h`.]*/
        tls_io_instance->on_io_open_complete(tls_io_instance->on_io_open_complete_context, open_result);
    }
}


static int lwip_net_errno(int fd)
{
    int sock_errno = 0;
    u32_t optlen = sizeof(sock_errno);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_errno, &optlen);
    return sock_errno;
}

static void lwip_set_non_block(int fd) 
{
  int flags = -1;
  int error = 0;

  while(1){
      flags = fcntl(fd, F_GETFL, 0);
      if (flags == -1){
          error = lwip_net_errno(fd);
          if (error != EINTR){
              break;
          }
      } else{
          break;
      }
  }

  while(1){
      flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      if (flags == -1) {
          error = lwip_net_errno(fd);
          if (error != EINTR){
              break;
          }
      } else{
          break;
      }
  }

}

LOCAL int openssl_thread_LWIP_CONNECTION(TLS_IO_INSTANCE* p)
{
    //(void*)printf("openssl_thread_LWIP_CONNECTION begin: %d \n", system_get_free_heap_size());
    int result;
    int ret;
    int sock;

    struct sockaddr_in sock_addr;
    fd_set readset;
    fd_set writeset;
    fd_set errset;

    ip_addr_t target_ip;
    SSL_CTX *ctx;
    SSL *ssl;

    TLS_IO_INSTANCE* tls_io_instance = p;

    // LogInfo("OpenSSL thread start...");

    int netconn_retry = 0;
    do {
        //(void*)printf("size before netconn_gethostbyname: %d \n", system_get_free_heap_size());
        /* Codes_SRS_TLSIO_SSL_ESP8266_99_018: [ The tlsio_openssl_open shall convert the provide hostName to an IP address. ]*/
        ret = netconn_gethostbyname(tls_io_instance->hostname, &target_ip);
    } while((ret != 0) && netconn_retry++ < MAX_RETRY);

    if (ret != 0){
        result = __LINE__;
        /* Codes_TLSIO_SSL_ESP8266_99_019: [ If the WiFi cannot find the IP for the hostName, the tlsio_openssl_open shall return __LINE__. ]*/
        LogError("Host %s not found.", tlsio_config->hostname);
    }
    else
    {
        // LogInfo("create socket ......");
        //(void*)printf("size before creating socket: %d \n", system_get_free_heap_size());
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            result = __LINE__;
            LogError("create socket failed");
        }
        else
        {
            tls_io_instance->sock = sock;
            LogInfo("sock: %d", sock);
            LogInfo("create socket OK");
            LogInfo("bind socket ......");
            memset(&sock_addr, 0, sizeof(sock_addr));
            sock_addr.sin_family = AF_INET;
            sock_addr.sin_addr.s_addr = 0;
            sock_addr.sin_port = htons(OPENSSL_LOCAL_TCP_PORT);
            int reuseAddr=1;
            ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));
            if (ret != 0) {
                result = __LINE__;
                LogError("setsockopt failed");
            }
            else {
                os_delay_us(10000000);
                ret = bind(sock, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
                
                //(void*)printf("bind return: %d \n", ret);
                if (ret != 0) {
                    result = __LINE__;
                    LogError("bind socket failed");
                }
                else
                {
                    memset(&sock_addr, 0, sizeof(sock_addr));
                    sock_addr.sin_family = AF_INET;
                    sock_addr.sin_addr.s_addr = target_ip.addr;
                    sock_addr.sin_port = htons(tls_io_instance->port);

                    lwip_set_non_block(sock);

                    ret = connect(sock, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
                    (void*)printf("connect return: %d \n", ret);
                    if (ret == -1) {
                        ret = lwip_net_errno(sock);
                        (void*)printf("lwip_net_errno ret: %d \n", ret);
                        if (ret != EINPROGRESS){
                            result = __LINE__;
                            ret = -1;
                            LogError("socket connect failed, not EINPROGRESS %s", tls_io_instance->hostname);
                            int close_ret = close(sock);
                            if (close_ret != 0){
                                LogError("close socket failed");
                                (void*)printf("close socket failed \n");
                            }
                        }
                    }

                    if(ret != -1 || ret == EINPROGRESS)
                    {
                        char recv_buf[128];
                        size_t recv_bytes = (size_t)sizeof(recv_buf);
                        int retry = 0;

                        while (retry < MAX_RETRY){
                            FD_ZERO(&readset);
                            FD_SET(sock, &readset);
                        
                            FD_ZERO(&writeset);
                            FD_SET(sock, &writeset);
                        
                            FD_ZERO(&errset);
                            FD_SET(sock, &errset);
                        
                            ret = lwip_select(sock + 1, &readset, &writeset, &errset, NULL);
                            if (ret > 0){
                                if (FD_ISSET(sock, &writeset)){
                                  break;
                                }
                        
                                if (FD_ISSET(sock, &readset)){
                                    memset(recv_buf, 0, recv_bytes);
                                    break;
                                }
                            }

                            retry++;
                            os_delay_us(RETRY_DELAY);
                        }

                        ctx = SSL_CTX_new(TLSv1_client_method());
                        if (ctx == NULL) {
                            result = __LINE__;
                            LogError("create new SSL CTX failed");
                        }
                        else
                        {
                            // LogInfo("create new SSL CTX OK");
                            // LogInfo("SSL set fragment");
                            ret = SSL_set_fragment(ctx, OPENSSL_FRAGMENT_SIZE);
                            //(void*)printf("SSL_set_fragment ret:%d \n", ret);
                            if (ret != 0){
                                result = __LINE__;
                                LogError("SSL_set_fragment failed");
                            }
                            else
                            {
                                // LogInfo("SSL new... ");
                                ssl = SSL_new(ctx);
                                //(void*)printf("after ssl new \n");
                                if (ssl == NULL) {
                                    result = __LINE__;
                                    LogError("create ssl failed");
                                }
                                else
                                {
                                    // LogInfo("SSL set fd");
                                    // returns 1 on success
                                    ret = SSL_set_fd(ssl, sock);
                                    //(void*)printf("SSL_set_fd ret:%d \n", ret);
                                    if (ret != 1){
                                        result = __LINE__;
                                        LogError("SSL_set_fd failed");
                                    }
                                    else{
                                        // LogInfo("SSL connect... ");
                                        int retry = 0;
                                        while (SSL_connect(ssl) != 0 && retry < MAX_RETRY)
                                        {  
                                            FD_ZERO(&readset);
                                            FD_SET(sock, &readset);
                                            FD_ZERO(&writeset);
                                            FD_SET(sock, &writeset);
                                            FD_ZERO(&errset);
                                            FD_SET(sock, &errset);

                                            lwip_select(sock + 1, &readset, &writeset, &errset, NULL);

                                            retry++;
                                            os_delay_us(RETRY_DELAY);
                                        }
                                        if (retry >= MAX_RETRY)
                                        {
                                            result = __LINE__;
                                        (void*)printf("SSL_connect failed \n");
                                        }else{
                                            // LogInfo("SSL connect ok");
                                            tls_io_instance->ssl = ssl;
                                            tls_io_instance->ssl_context = ctx;
                                            result = 0;
                                            //(void*)printf("SSL_connect succeed");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

            }
        }
    }
    //(void*)printf("openssl_thread_LWIP_CONNECTION end: %d \n", system_get_free_heap_size());
    return result;
}

static int send_handshake_bytes(TLS_IO_INSTANCE* tls_io_instance)
{
    //system_print_meminfo(); // This is useful for debugging purpose.
    //LogInfo("free heap size %d", system_get_free_heap_size()); // This is useful for debugging purpose.
    int result;
    if (openssl_thread_LWIP_CONNECTION(tls_io_instance) != 0){
        tls_io_instance->tlsio_state = TLSIO_STATE_ERROR;
        indicate_open_complete(tls_io_instance, IO_OPEN_ERROR); 
        result = __LINE__;
    }else{
        tls_io_instance->tlsio_state = TLSIO_STATE_OPEN;
        indicate_open_complete(tls_io_instance, IO_OPEN_OK);    
        result = 0;
    }

    return result;
}


static int decode_ssl_received_bytes(TLS_IO_INSTANCE* tls_io_instance)
{
    int result;
    unsigned char buffer[64];

    int rcv_bytes;
    rcv_bytes = SSL_read(tls_io_instance->ssl, buffer, sizeof(buffer));
    // LogInfo("decode ssl recv bytes: %d", rcv_bytes);
    if (rcv_bytes > 0)
    {
        /* Codes_SRS_TLSIO_SSL_ESP8266_99_070: [ If the tlsio state is TLSIO_STATE_OPEN, and there are received data in the ssl client, the tlsio_openssl_dowork shall read this data and call the on_bytes_received with the pointer to the buffer with the data. ]*/
        if (tls_io_instance->on_bytes_received == NULL)
        {
            LogError("NULL on_bytes_received.");
        }
        else
        {
            tls_io_instance->on_bytes_received(tls_io_instance->on_bytes_received_context, buffer, rcv_bytes);
        }
    }
    result = 0;
    return result;
}

static void destroy_openssl_instance(TLS_IO_INSTANCE* tls_io_instance)
{
    //(void*)printf("destroy begin: %d \n", system_get_free_heap_size());
    LogInfo("destroy_openssl_instance");
    if (tls_io_instance != NULL)
    {
        if (tls_io_instance->ssl != NULL)
        {
            SSL_free(tls_io_instance->ssl);
            tls_io_instance->ssl = NULL;
            LogInfo("SSL_free");
        }
        if (tls_io_instance->ssl_context != NULL)
        {
            SSL_CTX_free(tls_io_instance->ssl_context);
            tls_io_instance->ssl_context = NULL;
        }
        int close_ret = close(tls_io_instance->sock);
        if (close_ret != 0){
            LogError("close socket failed");
        }
        //(void*)printf("destroy end: %d \n", system_get_free_heap_size());
    }
}

static int add_certificate_to_store(TLS_IO_INSTANCE* tls_io_instance, const char* certValue)
{
    return 0;
}


static int create_openssl_instance(TLS_IO_INSTANCE* tlsInstance)
{
    return 0;
}

/* Codes_SRS_TLSIO_SSL_ESP8266_99_005: [ The tlsio_ssl_esp8266 shall received the connection information using the TLSIO_CONFIG structure defined in `tlsio.h`. ]*/
/* Codes_SRS_TLSIO_SSL_ESP8266_99_009: [ The tlsio_openssl_create shall create a new instance of the tlsio for esp8266. ]*/
/* Codes_SRS_TLSIO_SSL_ESP8266_99_017: [ The tlsio_openssl_create shall receive the connection configuration (TLSIO_CONFIG). ]*/
CONCRETE_IO_HANDLE tlsio_openssl_create(void* io_create_parameters)
{
    /* Codes_SRS_TLSIO_SSL_ESP8266_99_005: [ The tlsio_ssl_esp8266 shall received the connection information using the TLSIO_CONFIG structure defined in `tlsio.h`. ]*/
    /* Codes_SRS_TLSIO_SSL_ESP8266_99_017: [ The tlsio_openssl_create shall receive the connection configuration (TLSIO_CONFIG). ]*/
    TLSIO_CONFIG* tls_io_config = (TLSIO_CONFIG*)io_create_parameters;
    TLS_IO_INSTANCE* result;

    /* Codes_SRS_TLSIO_SSL_ESP8266_99_013: [ The tlsio_openssl_create shall return NULL when io_create_parameters is NULL. ]*/
    if (tls_io_config == NULL)
    {
        result = NULL;
        LogError("NULL tls_io_config.");
    }
    else
    {
        /* Codes_SRS_TLSIO_SSL_ESP8266_99_011: [ The tlsio_openssl_create shall allocate memory to control the tlsio instance. ]*/
        result = (TLS_IO_INSTANCE*) malloc(sizeof(TLS_IO_INSTANCE));

        if (result == NULL)
        {
            /* Codes_SRS_TLSIO_SSL_ESP8266_99_012: [ If there is not enough memory to create the tlsio, the tlsio_openssl_create shall return NULL as the handle. ]*/
            LogError("Failed allocating TLSIO instance.");
        }
        else
        {
            memset(result, 0, sizeof(TLS_IO_INSTANCE));
            mallocAndStrcpy_s(&result->hostname, tls_io_config->hostname);
            result->port = tls_io_config->port;
            result->ssl_context = NULL;
            result->ssl = NULL;
            result->certificate = NULL;

            /* Codes_SRS_TLSIO_SSL_ESP8266_99_016: [ The tlsio_openssl_create shall initialize all callback pointers as NULL. ]*/
            result->on_bytes_received = NULL;
            result->on_bytes_received_context = NULL;

            result->on_io_open_complete = NULL;
            result->on_io_open_complete_context = NULL;

            result->on_io_close_complete = NULL;
            result->on_io_close_complete_context = NULL;

            result->on_io_error = NULL;
            result->on_io_error_context = NULL;

            /* Codes_SRS_TLSIO_SSL_ESP8266_99_020: [ If tlsio_openssl_create get success to create the tlsio instance, it shall set the tlsio state as TLSIO_STATE_NOT_OPEN. ]*/
            result->tlsio_state = TLSIO_STATE_NOT_OPEN;

            result->x509certificate = NULL;
            result->x509privatekey = NULL;
        }
    }

    /* Codes_SRS_TLSIO_SSL_ESP8266_99_010: [ The tlsio_openssl_create shall return a non-NULL handle on success. ]*/
    return (CONCRETE_IO_HANDLE)result;
}

/* Codes_SRS_TLSIO_SSL_ESP8266_99_021: [ The tlsio_openssl_destroy shall destroy a created instance of the tlsio for esp8266 identified by the CONCRETE_IO_HANDLE. ]*/
void tlsio_openssl_destroy(CONCRETE_IO_HANDLE tls_io)
{
    /* Codes_SRS_TLSIO_SSL_ESP8266_99_024: [ If the tlsio_handle is NULL, the tlsio_openssl_destroy shall not do anything. ]*/
    if (tls_io == NULL)
    {
        LogError("NULL tls_io.");
    }
    else
    {
        TLS_IO_INSTANCE* tls_io_instance = (TLS_IO_INSTANCE*)tls_io;
        if (tls_io_instance->certificate != NULL)
        {
            free(tls_io_instance->certificate);
        }
        if (tls_io_instance->hostname != NULL)
        {
            free(tls_io_instance->hostname);
        }
        if (tls_io_instance->x509certificate != NULL)
        {
            free((void*)tls_io_instance->x509certificate);
        }
        if (tls_io_instance->x509privatekey != NULL)
        {
            free((void*)tls_io_instance->x509privatekey);
        }
        free(tls_io);
    }
}


/* Codes_SRS_TLSIO_SSL_ESP8266_99_008: [ The tlsio_openssl_open shall return 0 when succeed ]*/
int tlsio_openssl_open(CONCRETE_IO_HANDLE tls_io, ON_IO_OPEN_COMPLETE on_io_open_complete, void* on_io_open_complete_context, ON_BYTES_RECEIVED on_bytes_received, void* on_bytes_received_context, ON_IO_ERROR on_io_error, void* on_io_error_context)
{
    int result;

    if (tls_io == NULL)
    {
        /* Codes_SRS_TLSIO_SSL_ESP8266_99_006: [ The tlsio_openssl_open failed because tls_io is NULL. ]*/
        result = __LINE__;
        LogError("NULL tls_io.");
    }
    else
    {

        TLS_IO_INSTANCE* tls_io_instance = (TLS_IO_INSTANCE*)tls_io;

        /* Codes_SRS_TLSIO_SSL_ESP8266_99_007: [ The tlsio_openssl_open invalid state. ]*/
        if (tls_io_instance->tlsio_state != TLSIO_STATE_NOT_OPEN)
        {
            tls_io_instance->tlsio_state = TLSIO_STATE_ERROR;
            tls_io_instance->on_io_error = on_io_error;
            tls_io_instance->on_io_error_context = on_io_error_context;

            result = __LINE__;
            LogError("Invalid tlsio_state. Expected state is TLSIO_STATE_NOT_OPEN.");
            if (tls_io_instance->on_io_error != NULL)
            {
                tls_io_instance->on_io_error(tls_io_instance->on_io_error_context);
            }
        }
        else
        {
            /* Codes_SRS_TLSIO_SSL_ESP8266_99_004: [ The tlsio_ssl_esp8266 shall call the callbacks functions defined in the `xio.h`. ]*/
            /* Codes_SRS_TLSIO_SSL_ESP8266_99_006: [ The tlsio_ssl_esp8266 shall return the status of all async operations using the callbacks. ]*/
            /* Codes_SRS_TLSIO_SSL_ESP8266_99_007: [ If the callback function is set as NULL. The tlsio_ssl_esp8266 shall not call anything. ]*/
            tls_io_instance->on_io_open_complete = on_io_open_complete;
            tls_io_instance->on_io_open_complete_context = on_io_open_complete_context;

            tls_io_instance->on_bytes_received = on_bytes_received;
            tls_io_instance->on_bytes_received_context = on_bytes_received_context;

            tls_io_instance->on_io_error = on_io_error;
            tls_io_instance->on_io_error_context = on_io_error_context;

            tls_io_instance->tlsio_state = TLSIO_STATE_OPENING;

            if (send_handshake_bytes(tls_io_instance) != 0){
                result = __LINE__;
                tls_io_instance->tlsio_state = TLSIO_STATE_ERROR;
                LogError("send_handshake_bytes failed.");
                if (tls_io_instance->on_io_error != NULL)
                {
                    tls_io_instance->on_io_error(tls_io_instance->on_io_error_context);
                }
            }else{
                result = 0;
                tls_io_instance->tlsio_state = TLSIO_STATE_OPEN;
                os_delay_us(5000000); //delay added to give reconnect time to send last message
                //(void*)printf("tlsio_openssl_open end: %d \n", system_get_free_heap_size());
            }
        }
    }
    return result;
}


/* Codes_SRS_TLSIO_SSL_ESP8266_99_013: [ The tlsio_openssl_close succeed.]*/
int tlsio_openssl_close(CONCRETE_IO_HANDLE tls_io, ON_IO_CLOSE_COMPLETE on_io_close_complete, void* callback_context)
{
    //(void*)printf("tlsio_openssl_close begin: %d \n", system_get_free_heap_size());
    LogInfo("tlsio_openssl_close");
    int result;

    /* Codes_SRS_TLSIO_SSL_ESP8266_99_011: [ The tlsio_openssl_close NULL parameter.]*/
    if (tls_io == NULL)
    {
        result = __LINE__;
        LogError("NULL tls_io.");
    }
    else
    {
        TLS_IO_INSTANCE* tls_io_instance = (TLS_IO_INSTANCE*)tls_io;

        if ((tls_io_instance->tlsio_state == TLSIO_STATE_NOT_OPEN) ||
            (tls_io_instance->tlsio_state == TLSIO_STATE_CLOSING) ||
            (tls_io_instance->tlsio_state == TLSIO_STATE_OPENING))
        {
            result = __LINE__;
            tls_io_instance->tlsio_state = TLSIO_STATE_ERROR;
            LogError("Invalid tlsio_state. Expected state is TLSIO_STATE_OPEN or TLSIO_STATE_ERROR.");
        }
        else
        {
            tls_io_instance->tlsio_state = TLSIO_STATE_CLOSING;
            tls_io_instance->on_io_close_complete = on_io_close_complete;
            tls_io_instance->on_io_close_complete_context = callback_context;

            int ret = SSL_shutdown(tls_io_instance->ssl);
            (void*)printf("SSL_shutdown ret: %d \n", ret);
            if (ret != 0)
            {
                LogError("SSL_shutdown failed.");
            }
            destroy_openssl_instance(tls_io_instance);
            tls_io_instance->tlsio_state = TLSIO_STATE_NOT_OPEN;
            result = 0;
            if (tls_io_instance->on_io_close_complete != NULL)
            {
                tls_io_instance->on_io_close_complete(tls_io_instance->on_io_close_complete_context);
            }
        }
    }
    //(void*)printf("tlsio_openssl_close end: %d \n", system_get_free_heap_size());
    return result;
}

int tlsio_openssl_send(CONCRETE_IO_HANDLE tls_io, const void* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    //(void*)printf("tlsio_openssl_send \n");

    int result;

    if (tls_io == NULL)
    {
        /* Codes_SRS_TLSIO_SSL_ESP8266_99_014: [ The tlsio_openssl_send NULL instance.]*/
        result = __LINE__;
        LogError("NULL tls_io.");
    }
    else if (buffer == NULL)
    {
        /* Codes_SRS_TLSIO_SSL_ESP8266_99_014: [ The tlsio_openssl_send NULL instance.]*/
        result = __LINE__;
        LogError("NULL buffer.");
    }
    else
    {
        TLS_IO_INSTANCE* tls_io_instance = (TLS_IO_INSTANCE*)tls_io;

        if (tls_io_instance->tlsio_state != TLSIO_STATE_OPEN)
        {
            /* Codes_SRS_TLSIO_SSL_ESP8266_99_015: [ The tlsio_openssl_send wrog state.]*/
            result = __LINE__;
            LogError("Invalid tlsio_state. Expected state is TLSIO_STATE_OPEN.");
        }
        else
        {
            int total_write = 0;
            int res = 0;
            int retry = 0;

            while(size > 0 && retry < MAX_RETRY_WRITE){
                /* Codes_SRS_TLSIO_SSL_ESP8266_99_016: [ The tlsio_openssl_send SSL_write success]*/
                /* Codes_SRS_TLSIO_SSL_ESP8266_99_017: [ The tlsio_openssl_send SSL_write failure]*/
                res = SSL_write(tls_io_instance->ssl, ((uint8*)buffer)+total_write, size);
                //LogInfo("SSL_write res: %d, size: %d, retry: %d", res, size, retry);
                if(res > 0 && res <= size){
                    total_write += res;
                    size = size - res;
                }
                else
                {
                    retry++;
                    os_delay_us(100000);//100ms
                }
            }

            if (retry >= MAX_RETRY_WRITE)
            {
                result = __LINE__;
                if (on_send_complete != NULL)
                {
                    /* Codes_SRS_TLSIO_SSL_ESP8266_99_003: [ The tlsio_ssl_esp8266 shall report the send operation status using the IO_SEND_RESULT enumerator defined in the `xio.h`. ]*/
                    on_send_complete(callback_context, IO_SEND_ERROR);
                }
            }
            else
            {
                result = 0;
                if (on_send_complete != NULL)
                {
                    /* Codes_SRS_TLSIO_SSL_ESP8266_99_003: [ The tlsio_ssl_esp8266 shall report the send operation status using the IO_SEND_RESULT enumerator defined in the `xio.h`. ]*/
                    on_send_complete(callback_context, IO_SEND_OK);
                }
            }

            //(void*)printf("total write: %d \n", total_write);
        }
    }
    return result;
}

/* Codes_SRS_TLSIO_SSL_ESP8266_99_019: [ The tlsio_openssl_dowrok succeed]*/
void tlsio_openssl_dowork(CONCRETE_IO_HANDLE tls_io)
{
    if (tls_io == NULL)
    {
        /* Codes_SRS_TLSIO_SSL_ESP8266_99_018: [ The tlsio_openssl_dowork NULL parameter. No crash when passing NULL]*/
        LogError("NULL tls_io.");
    }
    else
    {
        TLS_IO_INSTANCE* tls_io_instance = (TLS_IO_INSTANCE*)tls_io;

        if (tls_io_instance->tlsio_state == TLSIO_STATE_OPEN)
        {
            decode_ssl_received_bytes(tls_io_instance);
        } 
        else
        {
            LogError("Invalid tlsio_state. Expected state is TLSIO_STATE_OPEN.");
        }
    }

}

/* Codes_SRS_TLSIO_SSL_ESP8266_99_002: [ The tlsio_openssl_setoption shall not do anything, and return 0. ]*/
int tlsio_openssl_setoption(CONCRETE_IO_HANDLE tls_io, const char* optionName, const void* value)
{
    return 0;
}

/* Codes_SRS_TLSIO_SSL_ESP8266_99_008: [ The tlsio_openssl_get_interface_description shall return the VTable IO_INTERFACE_DESCRIPTION. ]*/
const IO_INTERFACE_DESCRIPTION* tlsio_openssl_get_interface_description(void)
{
    return &tlsio_openssl_interface_description;
}
