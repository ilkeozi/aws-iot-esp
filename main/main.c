/* Standard includes. */
#include <string.h>

/* FreeRTOS includes. */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

/* ESP-IDF includes. */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
/* ESP Secure Certificate Manager include. */
#include "esp_secure_cert_read.h"

/* Network transport include. */
#include "network_transport.h"

/* coreMQTT-Agent network manager include. */
#include "core_mqtt_agent_manager.h"

/* WiFi provisioning/connection handler include. */
#include "app_wifi.h"
#include "pppos_client.h" 
/* Demo includes. */
#if CONFIG_PB_LED
    #include "pubsub.h"
    #include "led_control.h"
    #include "barrier_control.h"
    #include "barrier_driver.h"
    #include "ina3221_sensor.h"
    #include "power_perception.h"
    #include "obstacle_perception.h"
    #include "wifi_perception.h"
    #include "driver/gpio.h"
    #include "buzzer_control.h"



#endif /* CONFIG_GRI_ENABLE_SUB_PUB_UNSUB */

#if CONFIG_GRI_ENABLE_TEMPERATURE_PUB_SUB_AND_LED_CONTROL
    #include "temp_sub_pub_and_led_control.h"
#endif /* CONFIG_GRI_ENABLE_TEMPERATURE_PUB_SUB_AND_LED_CONTROL */

#if CONFIG_GRI_ENABLE_OTA
    #include "ota_pal.h"
    #include "ota_over_mqtt.h"
#endif /* CONFIG_GRI_ENABLE_OTA */

#if CONFIG_GRI_RUN_QUALIFICATION_TEST
    #include "qualification_wrapper_config.h"
#endif /* CONFIG_GRI_RUN_QUALIFICATION_TEST */

/**
 * @brief The AWS RootCA1 passed in from ./certs/root_cert_auth.pem
 */
extern const char root_cert_auth_start[] asm ( "_binary_root_cert_auth_crt_start" );
extern const char root_cert_auth_end[]   asm ( "_binary_root_cert_auth_crt_end" );

/* Global variables ***********************************************************/

/**
 * @brief Logging tag for ESP-IDF logging functions.
 */
static const char * TAG = "main";

/**
 * @brief The global network context used to store the credentials
 * and TLS connection.
 */
static NetworkContext_t xNetworkContext;

#if CONFIG_GRI_ENABLE_OTA

/**
 * @brief The AWS code signing certificate passed in from ./certs/aws_codesign.crt
 */
    extern const char pcAwsCodeSigningCertPem[] asm ( "_binary_aws_codesign_crt_start" );

#endif /* CONFIG_GRI_ENABLE_OTA */

/* Static function declarations ***********************************************/

/**
 * @brief This function initializes the global network context with credentials.
 *
 * This handles retrieving and initializing the global network context with the
 * credentials it needs to establish a TLS connection.
 */
static BaseType_t prvInitializeNetworkContext( void );

/**
 * @brief This function starts all enabled demos.
 */
static void prvStartEnabledDemos( void );

#if CONFIG_GRI_RUN_QUALIFICATION_TEST
    extern BaseType_t xQualificationStart( void );
#endif /* CONFIG_GRI_RUN_QUALIFICATION_TEST */

/* Static function definitions ************************************************/

static BaseType_t prvInitializeNetworkContext( void )
{
    /* This is returned by this function. */
    BaseType_t xRet = pdPASS;

    /* This is used to store the error return of ESP-IDF functions. */
    esp_err_t xEspErrRet;

    /* Verify that the MQTT endpoint and thing name have been configured by the
     * user. */
    if( strlen( CONFIG_GRI_MQTT_ENDPOINT ) == 0 )
    {
        ESP_LOGE( TAG, "Empty endpoint for MQTT broker. Set endpoint by "
                       "running idf.py menuconfig, then PB AWS Integration -> "
                       "Endpoint for MQTT Broker to use." );
        xRet = pdFAIL;
    }

    if( strlen( CONFIG_GRI_THING_NAME ) == 0 )
    {
        ESP_LOGE( TAG, "Empty thingname for MQTT broker. Set thing name by "
                       "running idf.py menuconfig, then PB AWS  Integration -> "
                       "Thing name." );
        xRet = pdFAIL;
    }

    /* Initialize network context. */

    xNetworkContext.pcHostname = CONFIG_GRI_MQTT_ENDPOINT;
    xNetworkContext.xPort = CONFIG_GRI_MQTT_PORT;

    /* Get the device certificate from esp_secure_crt_mgr and put into network
     * context. */
    xEspErrRet = esp_secure_cert_get_device_cert( &xNetworkContext.pcClientCert,
                                                  &xNetworkContext.pcClientCertSize );

    if( xEspErrRet == ESP_OK )
    {
        #if CONFIG_GRI_OUTPUT_CERTS_KEYS
            ESP_LOGI( TAG, "\nDevice Cert: \nLength: %" PRIu32 "\n%s",
                      xNetworkContext.pcClientCertSize,
                      xNetworkContext.pcClientCert );
        #endif /* CONFIG_GRI_OUTPUT_CERTS_KEYS */
    }
    else
    {
        ESP_LOGE( TAG, "Error in getting device certificate. Error: %s",
                  esp_err_to_name( xEspErrRet ) );

        xRet = pdFAIL;
    }

    /* Putting the Root CA certificate into the network context. */
    xNetworkContext.pcServerRootCA = root_cert_auth_start;
    xNetworkContext.pcServerRootCASize = root_cert_auth_end - root_cert_auth_start;

    if( xEspErrRet == ESP_OK )
    {
        #if CONFIG_GRI_OUTPUT_CERTS_KEYS
            ESP_LOGI( TAG, "\nCA Cert: \nLength: %" PRIu32 "\n%s",
                      xNetworkContext.pcServerRootCASize,
                      xNetworkContext.pcServerRootCA );
        #endif /* CONFIG_GRI_OUTPUT_CERTS_KEYS */
    }
    else
    {
        ESP_LOGE( TAG, "Error in getting CA certificate. Error: %s",
                  esp_err_to_name( xEspErrRet ) );

        xRet = pdFAIL;
    }

    #if CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL

        /* If the digital signature peripheral is being used, get the digital
         * signature peripheral context from esp_secure_crt_mgr and put into
         * network context. */

        xNetworkContext.ds_data = esp_secure_cert_get_ds_ctx();

        if( xNetworkContext.ds_data == NULL )
        {
            ESP_LOGE( TAG, "Error in getting digital signature peripheral data." );
            xRet = pdFAIL;
        }
    #else /* if CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL */
        xEspErrRet = esp_secure_cert_get_priv_key( &xNetworkContext.pcClientKey,
                                                   &xNetworkContext.pcClientKeySize );

        if( xEspErrRet == ESP_OK )
        {
            #if CONFIG_GRI_OUTPUT_CERTS_KEYS
                ESP_LOGI( TAG, "\nPrivate Key: \nLength: %" PRIu32 "\n%s",
                          xNetworkContext.pcClientKeySize,
                          xNetworkContext.pcClientKey );
            #endif /* CONFIG_GRI_OUTPUT_CERTS_KEYS */
        }
        else
        {
            ESP_LOGE( TAG, "Error in getting private key. Error: %s",
                      esp_err_to_name( xEspErrRet ) );

            xRet = pdFAIL;
        }
    #endif /* CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL */

    xNetworkContext.pxTls = NULL;
    xNetworkContext.xTlsContextSemaphore = xSemaphoreCreateMutex();

    if( xNetworkContext.xTlsContextSemaphore == NULL )
    {
        ESP_LOGE( TAG, "Not enough memory to create TLS semaphore for global "
                       "network context." );

        xRet = pdFAIL;
    }

    return xRet;
}

static void prvStartEnabledDemos( void )
{
    BaseType_t xResult;

    #if ( CONFIG_GRI_RUN_QUALIFICATION_TEST == 0 )
        

        /* Initialize and start the coreMQTT-Agent network manager. This handles
         * establishing a TLS connection and MQTT connection to the MQTT broker.
         * This needs to be started before starting WiFi so it can handle WiFi
         * connection events. */
        xResult = xCoreMqttAgentManagerStart( &xNetworkContext );

        if( xResult != pdPASS )
        {
            ESP_LOGE( TAG, "Failed to initialize and start coreMQTT-Agent network "
                           "manager." );

            configASSERT( xResult == pdPASS );
        }

        #if CONFIG_PB_LED
            vStartLEDControl();
            vStartBarrierControl();
            vStartPowerPerception();
            vStartObstaclePerception();
            vStartWifiPerception();
        #endif /* CONFIG_GRI_ENABLE_SIMPLE_PUB_SUB */

        #if CONFIG_GRI_ENABLE_TEMPERATURE_PUB_SUB_AND_LED_CONTROL
            vStartTempSubPubAndLEDControlDemo();
        #endif /* CONFIG_GRI_ENABLE_TEMPERATURE_LED_PUB_SUB */

        #if CONFIG_GRI_ENABLE_OTA
            #if CONFIG_GRI_OUTPUT_CERTS_KEYS
                ESP_LOGI( TAG, "\nCS Cert: \nLength: %zu\n%s",
                          strlen( pcAwsCodeSigningCertPem ),
                          pcAwsCodeSigningCertPem );
            #endif /* CONFIG_GRI_OUTPUT_CERTS_KEYS */

            if( otaPal_SetCodeSigningCertificate( pcAwsCodeSigningCertPem ) )
            {
                vStartOTACodeSigningDemo();
            }
            else
            {
                ESP_LOGE( TAG,
                          "Failed to set the code signing certificate for the AWS OTA "
                          "library. OTA demo will not be started." );
            }
        #endif /* CONFIG_GRI_ENABLE_OTA */

    #endif /* CONFIG_GRI_RUN_QUALIFICATION_TEST == 0 */

    #if CONFIG_GRI_RUN_QUALIFICATION_TEST
        /* Disable some logs to avoid failure on IDT log parser. */
        esp_log_level_set( "esp_ota_ops", ESP_LOG_NONE );
        esp_log_level_set( "esp-tls-mbedtls", ESP_LOG_NONE );
        esp_log_level_set( "AWS_OTA", ESP_LOG_NONE );

        if( ( xResult = xQualificationStart() ) != pdPASS )
        {
            ESP_LOGE( TAG, "Failed to start Qualification task: errno=%d", xResult );
        }
        configASSERT( xResult == pdPASS );
    #endif /* CONFIG_GRI_RUN_QUALIFICATION_TEST */
}

/**
 * @brief This function serves as the main entry point of this project.
 */
void app_main( void )
{
    
    /* This is used to store the return of initialization functions. */
    BaseType_t xRet;

    /* This is used to store the error return of ESP-IDF functions. */
    esp_err_t xEspErrRet;

    /* Initialize global network context. */
    xRet = prvInitializeNetworkContext();

    if( xRet != pdPASS )
    {
        ESP_LOGE( TAG, "Failed to initialize global network context." );
        return;
    }

    /* Initialize NVS partition. This needs to be done before initializing
     * WiFi. */
    xEspErrRet = nvs_flash_init();

    if( ( xEspErrRet == ESP_ERR_NVS_NO_FREE_PAGES ) ||
        ( xEspErrRet == ESP_ERR_NVS_NEW_VERSION_FOUND ) )
    {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK( nvs_flash_erase() );

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK( nvs_flash_init() );
    }


    ESP_ERROR_CHECK( esp_event_loop_create_default() );


    prvStartEnabledDemos();
    //pppos_start();

    /* Start WiFi. */
    app_wifi_init();
    app_wifi_start( POP_TYPE_MAC );

   
}

