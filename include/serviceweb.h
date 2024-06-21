#ifdef __cplusplus
extern "C"
{
#endif

void serviceweb_init(char* buffer, size_t size);
void serviceweb_start(void);
void serviceweb_stop(void);
void serviceweb_set_nvs_namespace(const char *name);

#ifdef __cplusplus
}
#endif