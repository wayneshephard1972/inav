
#include <stdint.h>
#include <stdlib.h>

#include "platform.h"

#include "usb_core.h"
#include "usb_init.h"
#include "hw_config.h"

#include <stdbool.h>
#include <string.h>


#include "drivers/system.h"

#include "serial.h"
#include "serial_usb_vcp.h"


#define USB_TIMEOUT  50

static vcpPort_t vcpPort;

void usbVcpSetBaudRate(serialPort_t *instance, uint32_t baudRate)
{
    // TODO implement
}

void usbVcpSetMode(serialPort_t *instance, portMode_t mode)
{
    // TODO implement
}

bool isUsbVcpTransmitBufferEmpty(serialPort_t *instance)
{
    return true;
}

uint8_t usbVcpAvailable(serialPort_t *instance)
{
    return receiveLength & 0xFF; // FIXME use uint32_t return type everywhere
}

uint8_t usbVcpRead(serialPort_t *instance)
{
    uint8_t buf[1];

    uint32_t rxed = 0;

    while (rxed < 1) {
        rxed += CDC_Receive_DATA((uint8_t*)buf + rxed, 1 - rxed);
    }

    return buf[0];
}

void usbVcpWrite(serialPort_t *instance, uint8_t c)
{
    uint32_t txed;
    uint32_t start = millis();

    if (!(usbIsConnected() && usbIsConfigured())) {
        return;
    }

    do {
        txed = CDC_Send_DATA((uint8_t*)&c, 1);
    } while (txed < 1 && (millis() - start < USB_TIMEOUT));

}

const struct serialPortVTable usbVTable[] = { { usbVcpWrite, usbVcpAvailable, usbVcpRead, usbVcpSetBaudRate, isUsbVcpTransmitBufferEmpty, usbVcpSetMode } };

serialPort_t *usbVcpOpen(void)
{
    vcpPort_t *s;

    Set_System();
    Set_USBClock();
    USB_Interrupts_Config();
    USB_Init();

    s = &vcpPort;
    s->port.vTable = usbVTable;

    return (serialPort_t *)s;
}