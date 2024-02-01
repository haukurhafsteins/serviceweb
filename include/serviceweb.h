#ifdef __cplusplus
extern "C"
{
#endif
void serviceweb_init(const char* nvs_namespace);
void serviceweb_start(void);
void serviceweb_stop(void);
bool serviceweb_fileserver_start(const char* base_path);
#ifdef __cplusplus
}
#endif