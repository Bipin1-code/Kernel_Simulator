
#include "pci_sim.h"
#include "pci_logic.h"
#include "OS_device.h"
#include "basic_driver.h"

#include <stdio.h>

int main(){
    puts("********KERNEL MAIN:********");

    printf("\n[SETUP] Initializing PCI simulator...\n");
    PciDevicePool();
    DriverPool();
    OS_InitPciCallbacks();
    DriverFrameworkInit();
    PciSetDeviceCallback(OS_HandleNewPciDevice);
    PciEnumerate();
    
    return 0;
}
