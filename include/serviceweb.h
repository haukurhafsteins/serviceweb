#pragma once
#ifdef __cplusplus
#include "pp.h"
extern "C"
{
#endif

void serviceweb_init(pp_evloop_t *evloop, char* txbuf, size_t size, char* rxbuf, size_t rxsize);
void serviceweb_start(void);
void serviceweb_stop(void);
void serviceweb_set_nvs_namespace(const char *name);
bool serviceweb_register_memory_file(const char* path, const uint8_t *start, const uint8_t *end, bool gzip);
void serviceweb_register_files(const char *basePath, const char *path);
void serviceweb_set_debug(bool enable);


#ifdef __cplusplus
}
#endif