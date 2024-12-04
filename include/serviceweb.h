#pragma once
#ifdef __cplusplus
extern "C"
{
#endif

void serviceweb_init(char* buffer, size_t size, char* rxbuf, size_t rxsize);
void serviceweb_start(void);
void serviceweb_stop(void);
void serviceweb_set_nvs_namespace(const char *name);
bool serviceweb_register_file(const char* path, const uint8_t *dir_html_start, const uint8_t *dir_html_end);

#ifdef __cplusplus
}
#endif