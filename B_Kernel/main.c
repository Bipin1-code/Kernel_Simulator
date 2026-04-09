
#include "pci_sim.h"
#include "pci_logic.h"
#include <stdio.h>

int main(){
    puts("********KERNEL MAIN:********");
    PciDevicePool();
    PciEnumerate();
    return 0;
}
