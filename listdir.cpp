#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_err.h"

static bool listFilesRecursively(const char *basePath)
{
    printf("basePath: %s\n", basePath);
    struct dirent *dp;
    DIR *dir = opendir(basePath);

    // Unable to open directory stream
    if (!dir)
        return false;

    char *path = (char *)malloc(1000 * sizeof(char));

    while ((dp = readdir(dir)) != NULL)
    {
        if (strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..") != 0)
        {
            // Construct new path from our base path
            strcpy(path, basePath);
            strcat(path, "/");
            strcat(path, dp->d_name);

            struct stat path_stat;
            stat(path, &path_stat);

            if (S_ISREG(path_stat.st_mode))
            {
                printf("%s\n", path);
            }
            else if (S_ISDIR(path_stat.st_mode))
            {
                listFilesRecursively(path);
            }
        }
    }

    free(path);
    closedir(dir);

    return true;
}

esp_err_t file_listdir_handler(httpd_req_t *req)
{
    listFilesRecursively(req->uri + strlen("/listdir"));
    return ESP_OK;
}
