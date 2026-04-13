
#include "pci_sim.h"
#include "pci_logic.h"
#include "OS_device.h"
#include "basic_driver.h"
#include <stdio.h>

int main(){
    puts("********KERNEL MAIN:********");
    PciDevicePool();
    DriverPool();
    DriverFrameworkInit();
    PciSetDeviceCallback(OS_HandleNewPciDevice);
    PciEnumerate();
    return 0;
}
