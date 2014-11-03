#include <mpd/idle.h>
#include <mpd/tag.h>
#include <mpd/status.h>
#include <mpd/client.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/types.h>

struct mpd_connection *conn;
typedef struct mpd_song mpd_song_t;
typedef struct mpd_status mpd_status_t;

#define MAINLOOP_ERROR do { handle_error(conn); return; } while (0)
#define MAX_HASH 1021
#define MAX_INFO_BUFF 256
#define MAX_OUTPUT_BUFF 2048

const char *host = "192.168.248.130";
unsigned int port = 6600;
const char *state_name[4] = {"unknown", "stopped", "now playing", "paused"};
const char *shell = "bash";
const char *trigger_command = "terminal-notifier -title '{title}: {state} ({elapsed_pct}%)' "
                            "-subtitle '{artist}' -message $'{album}\\\\n{track}' -sender com.apple.iTunes";

typedef const char *(*Hook_t)(mpd_status_t status, mpd_song_t song);
typedef struct Entry {
    const char *name;
    const char **content_ref;
    struct Entry *next;
} Entry;

typedef struct {
    Entry *head[MAX_HASH];
} HashTable;

HashTable *hash_table_create() {
    HashTable *ht = (HashTable *)malloc(sizeof(HashTable));
    memset(ht->head, sizeof(ht->head), 0);
    return ht;
}

unsigned hash_table_hash_func(const char *name) {
    unsigned int seed = 131, res = 0;
    while (*name)
        res = res * seed + (*name++ - 'a');
    return res % MAX_HASH;
}

void hash_table_destroy(HashTable *ht) {
    unsigned int i;
    for (i = 0; i < MAX_HASH; i++)
    {
        Entry *e, *ne;
        for (e = ht->head[i]; e; e = ne)
        {
            ne = e->next;
            free(e);
        }
    }
}

void hash_table_register(HashTable *ht, const char *name, const char **content_ref) {
    unsigned int hv = hash_table_hash_func(name);
    Entry *e = (Entry *)malloc(sizeof(Entry));
    e->name = name;
    e->content_ref = content_ref;
    e->next = ht->head[hv];
    ht->head[hv] = e;
}

const char *hash_table_lookup(HashTable *ht, const char *name) {
    unsigned int hv = hash_table_hash_func(name);
    Entry *e;
    for (e = ht->head[hv]; e; e = e->next)
        if (!strcmp(e->name, name))
            return *(e->content_ref);
    return NULL;
}

char etime_buff[MAX_INFO_BUFF], ttime_buff[MAX_INFO_BUFF], epct_buff[MAX_INFO_BUFF];
const char *title, *artist, *album, *track, *state,
      *elapsed_time = etime_buff, *total_time = ttime_buff, *elapsed_pct = epct_buff;

HashTable *dict;

void handle_error(struct mpd_connection *conn) {
    fprintf(stderr, "%s\n", mpd_connection_get_error_message(conn));
    mpd_connection_free(conn);
}

const char *filter(const char *input) {
    static char output_buff[MAX_OUTPUT_BUFF]; 
    static char token_buff[MAX_INFO_BUFF];
    char *optr = output_buff, *tptr = token_buff;
    enum {
        ESCAPE,
        IN_TOKEN,
        NORMAL
    } state = NORMAL;
    while (*input)
    {
        if (state == NORMAL)
        {
            if (*input == '\\')
                state = ESCAPE;
            else if (*input == '{')
                state = IN_TOKEN;
            else
                *optr++ = *input;
        }
        else if (state == IN_TOKEN)
        {
            if (*input == '}')
            {
                size_t csize;
                const char *content;
                *tptr = '\0'; /* mark the end of the token */
                if ((content = hash_table_lookup(dict, token_buff)))
                {
                    csize = strlen(content);
                    memmove(optr, content, csize);
                    optr += csize;
                }
                tptr = token_buff;
                state = NORMAL;
            }
            else *tptr++ = *input;
        }
        else
        {
            *optr++ = *input;
            state = NORMAL;
        }
        input++;
    }
    *optr = '\0';
    return output_buff;
}

void trigger(const char *filtered_cmd) {
    pid_t pid;
    int fd[2];
    pipe(fd);
    fprintf(stderr, "executing: %s\n", filtered_cmd);
    if ((pid = fork()) == -1)
    {
        fprintf(stderr, "failed to fork\n");
        return;
    }
    else if (!pid)
    {
        dup2(fd[0], 0); /* override stdin */
        close(fd[0]);
        close(fd[1]);
        execlp(shell, shell, (char *)NULL);
    }
    close(fd[0]);
    write(fd[1], filtered_cmd, strlen(filtered_cmd));
    close(fd[1]);
    waitpid(pid, NULL, 0);
}

void main_loop() {
    for (;;)
    {
        int idle_info;
        mpd_song_t *song;
        mpd_status_t *status;

        if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS)
            MAINLOOP_ERROR;

        mpd_send_idle_mask(conn, MPD_IDLE_PLAYER);
        idle_info = mpd_recv_idle(conn, 1);
        if (!idle_info) MAINLOOP_ERROR;
        fprintf(stderr, "new event: %s(%d)\n", mpd_idle_name(idle_info), idle_info);
        if (idle_info == MPD_IDLE_PLAYER)
        {
            int et, tt;
            mpd_send_status(conn); 
            status = mpd_recv_status(conn);
            if (!status) MAINLOOP_ERROR;
            state = state_name[mpd_status_get_state(status)];
            et = mpd_status_get_elapsed_time(status);
            tt = mpd_status_get_total_time(status);
            sprintf(etime_buff, "%i", et);
            sprintf(ttime_buff, "%i", tt);
            sprintf(epct_buff, "%d", tt ? et * 100 / tt : 0);
            mpd_status_free(status);
            mpd_send_current_song(conn);
            while ((song = mpd_recv_song(conn)) != NULL)
            {
                title = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
                artist = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0);
                album = mpd_song_get_tag(song, MPD_TAG_ALBUM, 0);
                track = mpd_song_get_tag(song, MPD_TAG_TRACK, 0);
                trigger(filter(trigger_command));
                mpd_song_free(song);
            }
        }
    }
}

int main() {
    int i;
    dict = hash_table_create();
    hash_table_register(dict, "title", &title);
    hash_table_register(dict, "artist", &artist);
    hash_table_register(dict, "album", &album);
    hash_table_register(dict, "track", &track);
    hash_table_register(dict, "state", &state);
    hash_table_register(dict, "elapsed_time", &elapsed_time);
    hash_table_register(dict, "total_time", &total_time);
    hash_table_register(dict, "elapsed_pct", &elapsed_pct);
    for (;;)
    {
        fprintf(stderr, "trying to connect %s:%d\n", host, port);
        conn = mpd_connection_new(host, port, 0);
        main_loop();
        fprintf(stderr, "reconnecting\n");
        sleep(2);
    }
}
