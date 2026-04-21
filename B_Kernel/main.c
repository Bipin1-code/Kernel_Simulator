
#include "pci_sim.h"
#include "pci_logic.h"
#include "OS_device.h"
#include "basic_driver.h"
#include <stdio.h>

int main(){
    puts("********KERNEL MAIN:********");

    puts("\n[SETUP] Initializing PCI simulator...\n");
    PciDevicePool();
    DriverPool();
    PciSetDeviceCallback(OS_HandleNewPciDevice);
    OS_InitPciCallbacks();
    DriverFrameworkInit();
    PciEnumerate();
    
    return 0;
}
