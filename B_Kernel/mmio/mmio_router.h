
#ifndef MMIO_ROUTER_H
#define MMIO_ROUTER_H

#ifdef __cplusplus
extern "c" {
#endif

#include <stdint.h>
#include <stdbool.h>

    typedef struct _MMIO_MATCH{
        uint8_t bus;
        uint8_t dev;
        uint8_t func;
        int bar_index;
        uint64_t bar_base;
        uint64_t bar_size;
        uint64_t offset;
        bool is_io_space;
        bool is_64bit;
    } MMIO_MATCH;

    //Main routing functions
    uint64_t MmioRouteRead(uint64_t phys_addr, int size_bytes);
    void MmioRouteWrite(uint64_t phys_addr, uint64_t value, int size_bytes);

    //debug function
    MMIO_MATCH* MmioFindDevice(uint64_t phys_addr, bool is_io_space);
    void MmioPrintMatch(MMIO_MATCH *match);
    
#ifdef __cplusplus
}
#endif

#endif
