#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


#define DEBUG_PRINT(...) do { printf( __VA_ARGS__); } while (0)
#define ADDONS_MAX 512
#define ERROR_EXIT(...) do { fprintf(stderr, __VA_ARGS__); chdir(initial_dir); exit(1); } while (0)
#define BUFFER_SIZE PATH_MAX + 512
#define RUN_IN_DIR(dir, ...) \
    do { \
        chdir(dir); \
        __VA_ARGS__ \
        chdir(working_dir); \
    } while (0)


struct Addon {
    char* name;
    char* path;
    char* repo;
    char* url;
    char* post;
    char* build_dir;
    bool  git;
};


char*   working_dir = NULL;
char*   initial_dir = NULL;
char*   addons_dir = "./addons";
char*   repos_dir  = "./repos";
char*   output_dir = "./output";

char*   script_make_release = "./utils/make-release.sh";
char*   script_is_behind    = "./utils/is-behind.sh";

struct Addon** addons = NULL;
int            addons_count = 0;


void* xmalloc(size_t size) {
    void* ptr = malloc(size);

    if (ptr == NULL)
        ERROR_EXIT("Failed to allocate memory\n");

    return ptr;
}


void* xcalloc(size_t nmemb, size_t size) {
    void* ptr = calloc(nmemb, size);

    if (ptr == NULL)
        ERROR_EXIT("Failed to allocate memory\n");

    return ptr;
}


void* xrealpath(char* path) {
    char* ptr = realpath(path, NULL);

    if (ptr == NULL)
        ERROR_EXIT("Failed to allocate memory\n");

    return ptr;
}


void* xstrdup(char* str) {
    char* ptr = strdup(str);

    if (ptr == NULL)
        ERROR_EXIT("Failed to allocate memory\n");

    return ptr;
}


char* path_join(char* buf, char* a, char* b) {
    snprintf(buf, PATH_MAX, "%s/%s", a, b);
    return buf;
}


char* path_join_alloced(char* a, char* b) {
    char buf[PATH_MAX] = {0};
    snprintf(buf, sizeof buf, "%s/%s", a, b);
    return xstrdup(buf);
}


bool path_exists(char* path) {
    return access(path, F_OK) != -1;
}


/* I'd greatly prefer to just check for a "." at the start, but
   there's the chance of some poorly designed addon breaking. */
bool is_ignored_file(char* file) {
    return strcmp(file, ".git") == 0 || strcmp(file, ".github") == 0||
           strcmp(file, ".") == 0    || strcmp(file, "..") == 0;
}


char* read_file(char* path) {
    FILE* file = fopen(path, "r");

    if (file == NULL)
        ERROR_EXIT("Failed to open file %s\n", path);

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char* buffer = xcalloc(size + 1, sizeof *buffer);
    fread(buffer, size, 1, file);
    fclose(file);

    return buffer;
}


char* read_line(char* str) {
    char buf[PATH_MAX] = {0};
    char* bufptr = buf;

    while (*str && *str != '\n')
        *(bufptr++) = *(str++);

    return xstrdup(buf);
}


void walk_dir(char* src, char* dest, bool dirs_last, void (*callback)(char* src, char* dest, bool is_dir)) {
    DIR* dir = opendir(src);
    struct dirent* entry = NULL;

    if (dir == NULL)
        ERROR_EXIT("Failed to open directory %s\n", src);

    if (!dirs_last)
        callback(src, dest, true);

    while ((entry = readdir(dir))) {
        if (is_ignored_file(entry->d_name))
            continue;

        char* new_src = path_join_alloced(src, entry->d_name);
        char* new_dest = dest ? path_join_alloced(dest, entry->d_name) : NULL;

        if (entry->d_type == DT_DIR) {
            walk_dir(new_src, new_dest, dirs_last, callback);
        } else if (entry->d_type == DT_REG) {
            callback(new_src, new_dest, false);
        } else {
            ERROR_EXIT("File %s is not a directory or regular file\n", new_src);
        }

        free(new_src);
        free(new_dest);
    }

    if (dirs_last)
        callback(src, dest, true);

    closedir(dir);
}


void copy_file(char* src, char* dest) {
    char buffer[4096] = {0};
    size_t bytes_read = 0;

    FILE* src_file = fopen(src, "r");
    FILE* dest_file = fopen(dest, "w");

    if (src_file == NULL)
        ERROR_EXIT("Failed to open file %s\n", src);

    if (dest_file == NULL)
        ERROR_EXIT("Failed to open file %s\n", dest);


    while ((bytes_read = fread(buffer, 1, sizeof buffer, src_file)) > 0) {
        fwrite(buffer, 1, bytes_read, dest_file);
    }

    fclose(src_file);
    fclose(dest_file);
}


void callback_copy(char* src, char* dest, bool is_dir) {
    if (is_dir) {
        if (!path_exists(dest) && mkdir(dest, 0755) != 0)
            ERROR_EXIT("Failed to create directory %s\n", dest);
    } else {
        copy_file(src, dest);
    }
}


void callback_delete(char* src, char* dest, bool is_dir) {
    if (is_dir) {
        if (rmdir(src) != 0)
            ERROR_EXIT("Failed to remove directory %s\n", src);
    } else {
        if (unlink(src) != 0)
            ERROR_EXIT("Failed to remove file %s\n", src);
    }
}


struct Addon* Addon_Create(char* name) {
    struct Addon* addon = xmalloc(sizeof *addon);
    char buf[PATH_MAX] = {0};

    addon->name = xstrdup(name);
    addon->path = path_join_alloced(addons_dir, name);
    addon->repo = path_join_alloced(repos_dir, name);

    char* config_path = path_join(buf, addon->path, name);
    strcat(config_path, ".addon");

    if (path_exists(config_path)) {
        char* url_start = strstr(read_file(config_path), "URL=");
        char* git_start = strstr(read_file(config_path), "GIT=");

        if (url_start && git_start) {
            addon->url = read_line(url_start + 4);
            addon->git = strcmp(read_line(git_start + 4), "true") == 0;
        } else {
            ERROR_EXIT("Invalid .addon file for addon %s\n", addon->name);
        }
    } else {
        ERROR_EXIT("No .addon file for addon %s\n", addon->name);
    }

    char* post_path = path_join(buf, addon->path, "post.sh");
    if (path_exists(post_path)) {
        addon->post = xrealpath(post_path);
    } else {
        addon->post = NULL;
    }

    addon->build_dir = path_join_alloced(addon->repo, "/.release");
    return addon;
}


void Addon_DeleteRepo(struct Addon* addon) {
    walk_dir(addon->repo, NULL, true, callback_delete);
}


int Addon_Merge(struct Addon* addon) {
    int status;

    RUN_IN_DIR(addon->repo,
        if (addon->git) {
            status = system("git merge origin");
        } else {
            chdir(working_dir);
            ERROR_EXIT("Non-git repos not supported yet\n");
        }
    );

    return status;
}


bool Addon_HasUpdate(struct Addon* addon) {
    bool is_behind;

    RUN_IN_DIR(addon->repo,
        if (addon->git) {
            system("git fetch origin");
            is_behind = system(script_is_behind);
        } else {
            chdir(working_dir);
            ERROR_EXIT("Non-git repos not supported yet\n");
        }
    );

    return is_behind;
}


bool Addon_HasRepo(struct Addon* addon) {
    return path_exists(addon->repo);
}


int Addon_Clone(struct Addon* addon) {
    char cmd[BUFFER_SIZE] = {0};

    if (addon->git)
        sprintf(cmd, "git clone %s %s", addon->url, addon->repo);
    else
        ERROR_EXIT("Non-git repos not supported yet\n");

    return system(cmd);
}


bool Addon_HasPkgMeta(struct Addon* addon) {
    char buf[PATH_MAX] = {0};
    if (path_exists(path_join(buf, addon->repo, ".pkgmeta")))     return true;
    if (path_exists(path_join(buf, addon->repo, "pkgmeta.yaml"))) return true;
    return false;
}


void Addon_Build(struct Addon* addon) {
    /* check if post is executable and try to make it if it isn't */
    if (addon->post && access(addon->post, X_OK) == -1) {
        struct stat statbuf;

        if (stat(addon->post, &statbuf) != 0)
            ERROR_EXIT("Failed to stat post script of %s\n", addon->name);

        if (chmod(addon->post, statbuf.st_mode | S_IXUSR) != 0)
            ERROR_EXIT("Failed to make post script of %s executable\n", addon->name);
    }

    RUN_IN_DIR(addon->repo,
        if (addon->post && system(addon->post) != 0)
            ERROR_EXIT("Post script for addon %s returned non-zero\n", addon->name);

        if (Addon_HasPkgMeta(addon)) {
            if (system(script_make_release) != 0)
                ERROR_EXIT("Failed to make release for addon %s\n", addon->name);
        }
    );
}


int Addon_MakeRelease(struct Addon* addon) {
    int status;

    RUN_IN_DIR(addon->repo,
        status = system(script_make_release);
    );

    return status;
}


void Addon_Print(struct Addon* addon) {
    printf("Addon: %s\n", addon->name);
    if (addon->path) printf("Path: %s\n", addon->path);
    if (addon->repo) printf("Repo: %s\n", addon->repo);
    if (addon->url)  printf("URL: %s\n", addon->url);
    if (addon->git)  printf("Git: true\n");
    if (addon->post) printf("Post: true\n");
}


void get_addons(void) {
    DIR* dir = opendir(addons_dir);
    struct dirent* entry = NULL;

    if (dir == NULL)
        ERROR_EXIT("Failed to open addons directory\n");

    if (addons != NULL)
        ERROR_EXIT("Addons already initialized\n");

    addons = xcalloc(ADDONS_MAX, sizeof *addons);

    while ((entry = readdir(dir))) {
        if (is_ignored_file(entry->d_name))
            continue;

        addons[addons_count++] = Addon_Create(entry->d_name);
    }

    if (addons_count == 0)
        ERROR_EXIT("No addons found\n");

    closedir(dir);
}


void Addon_CopyToOutput(struct Addon* addon) {
    DIR* dir = opendir(addon->build_dir);
    struct dirent* entry = NULL;

    if (dir == NULL)
        ERROR_EXIT("Failed to open build directory for addon %s\n", addon->name);

    while ((entry = readdir(dir))) {
        if (is_ignored_file(entry->d_name))
            continue;

        if (entry->d_type == DT_DIR) {
            char new_src[PATH_MAX] = {0};
            char new_dest[PATH_MAX] = {0};

            path_join(new_src, addon->build_dir, entry->d_name);
            path_join(new_dest, output_dir, entry->d_name);

            /* delete the destination dir just in case */
            if (path_exists(new_dest))
                walk_dir(new_dest, NULL, true, callback_delete);

            walk_dir(new_src, new_dest, false, callback_copy);
        }
    }

    closedir(dir);
}


void Addon_InstallOrUpdate(struct Addon* addon) {
    if (!Addon_HasRepo(addon)) {
        DEBUG_PRINT("Addon %s does not have a repository, cloning...\n", addon->name);

        if (Addon_Clone(addon) != 0)
            ERROR_EXIT("Failed to clone addon %s\n", addon->name);

        goto build_and_copy;
    }

    if (Addon_HasUpdate(addon)) {
        DEBUG_PRINT("Addon %s is behind, pulling...\n", addon->name);

        if (Addon_Merge(addon) != 0) {
            ERROR_EXIT("Failed to pull addon %s, deleting and cloning repo instead\n", addon->name);
            Addon_DeleteRepo(addon);

            if (Addon_Clone(addon) != 0)
                ERROR_EXIT("Failed to clone addon %s\n", addon->name);
        }
    } else {
        DEBUG_PRINT("Addon %s is up to date\n", addon->name);
        return;
    }


build_and_copy:
    Addon_Build(addon);
    Addon_CopyToOutput(addon);
}


void install_or_update_addons(void) {
    for (int i = 0; i < addons_count; i++) {
        if (addons[i] == NULL)
            ERROR_EXIT("Addon %d is NULL\n", i);

        if (fork() == 0) {
            Addon_InstallOrUpdate(addons[i]);
            exit(0);
        }
    }

    while (wait(NULL) > 0)
        ;

    DEBUG_PRINT("Done\n");
}


int main(int argc, char** argv) {
    char buf[PATH_MAX] = {0};
    initial_dir = getcwd(NULL, 0);

    if (readlink("/proc/self/exe", buf, PATH_MAX) == -1)
        ERROR_EXIT("Failed to get path to executable\n");

    if ((working_dir = strdup(dirname(buf))) == NULL)
        ERROR_EXIT("Failed to get working directory\n");

    if (chdir(working_dir) != 0)
        ERROR_EXIT("Failed to change working directory\n");

    if (!path_exists(repos_dir) && mkdir(repos_dir, 0755) != 0)
        ERROR_EXIT("Failed to create repos directory\n");

    output_dir          = xrealpath(output_dir);
    addons_dir          = xrealpath(addons_dir);
    repos_dir           = xrealpath(repos_dir);
    script_is_behind    = xrealpath(script_is_behind);
    script_make_release = xrealpath(script_make_release);

    if (!path_exists(output_dir))           ERROR_EXIT("Output directory does not exist\n");
    if (!path_exists(addons_dir))           ERROR_EXIT("Addons directory does not exist\n");
    if (!path_exists(script_is_behind))     ERROR_EXIT("is-behind.sh does not exist\n");
    if (!path_exists(script_make_release))  ERROR_EXIT("make-release.sh does not exist\n");

    get_addons();

    if (argc > 1) {
        if (strcmp(argv[1], "list") == 0) {
            for (int i = 0; i < addons_count; i++) {
                Addon_Print(addons[i]);
                printf("\n");
            }
        }
    } else {
        install_or_update_addons();
    }

    chdir(initial_dir);
    return 0;
}
