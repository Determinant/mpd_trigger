/**
 *  mpd_trigger: Execute whatever you want when MPD (Music Player Daemon) changes its state
 *  Copyright (C) 2014 Ted Yin

 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.

 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.

 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <getopt.h>

#include <mpd/idle.h>
#include <mpd/tag.h>
#include <mpd/status.h>
#include <mpd/client.h>

struct mpd_connection *conn;
typedef struct mpd_song mpd_song_t;
typedef struct mpd_status mpd_status_t;

#define MAINLOOP_ERROR do { handle_error(conn); return; } while (0)
#define MAX_HASH 1021
#define MAX_INFO_BUFF 256
#define MAX_OUTPUT_BUFF 2048

const char *host = "localhost";
unsigned int port = 6600;
unsigned int reconnect_time = 3;
const char *state_name[4] = {"unknown", "stopped", "now playing", "paused"};
const char *shell = "bash";
const char *trigger_command = "terminal-notifier -title \"{title}: {state} ({elapsed_pct}%)\" "
                            "-subtitle \"{artist}\" -message \"{album} @ {track?{track}:unknown track}\" -sender com.apple.iTunes";

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
    free(ht);
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
    fprintf(stderr, "[mpd] error: %s\n", mpd_connection_get_error_message(conn));
    mpd_connection_free(conn);
}

#define CHECK_OVERFLOW(ptr, buff, limit) \
    do { \
        if (ptr >= buff + limit) \
        { \
            fprintf(stderr, "[main] string is too long to be stored"); \
            exit(1); \
        } \
    } while (0)

const char *substitution(const char *exp, size_t *size) {
    static char output_buff[MAX_OUTPUT_BUFF];
    const char *content;
    char *optr = output_buff;
    char *cond_pos = NULL, *sep_pos = NULL;
    *size = 0;
    while (*exp)
    {
        CHECK_OVERFLOW(optr, output_buff, MAX_OUTPUT_BUFF);
        if (*exp == '?') cond_pos = optr;
        else if (*exp == ':') sep_pos = optr;
        *optr++ = *exp++; 
        (*size)++;
    }
    CHECK_OVERFLOW(optr, output_buff, MAX_OUTPUT_BUFF);
    *optr = '\0';
    if (cond_pos && sep_pos) /* a substitution */
    {
        char *start;
        *cond_pos = '\0';
        *sep_pos = '\0';
        content = hash_table_lookup(dict, output_buff);
        start = (content && *content) ? cond_pos + 1: sep_pos + 1;
        *size = strlen(start);
        memmove(output_buff, start, *size);
        output_buff[*size] = '\0';    
    }
    else
    {
        if ((content = hash_table_lookup(dict, output_buff)))
        {
            *size = strlen(content);
            memmove(output_buff, content, *size);
            output_buff[*size] = '\0';
        }
    }
    return output_buff;
}

const char *filter(const char *input) {
    static char output_buff[MAX_OUTPUT_BUFF]; 
    static char *pos[MAX_OUTPUT_BUFF];
    char *optr = output_buff;
    char **pptr = pos - 1;
    enum {
        ESCAPE,
        NORMAL
    } state = NORMAL;
    while (*input)
    {
        CHECK_OVERFLOW(optr, output_buff, MAX_OUTPUT_BUFF);
        if (state == NORMAL)
        {
            if (*input == '\\')
                state = ESCAPE;
            else if (*input == '}')
            {
                size_t csize;
                const char *content;
                char *bptr = optr - 1;
                if (pptr >= pos)
                {
                    while (bptr != *pptr) bptr--;
                    *optr = '\0';
                    content = substitution(bptr + 1, &csize);
                    memmove(bptr, content, csize);
                    optr = bptr + csize;
                    pptr--;
                }
                else *optr++ = '}';
                state = NORMAL;
            }
            else
            {
                if (*input == '{')
                    *(++pptr) = optr;
                *optr++ = *input;
            }
        }
        else
        {
            *optr++ = *input;
            state = NORMAL;
        }
        input++;
    }
    CHECK_OVERFLOW(optr, output_buff, MAX_OUTPUT_BUFF);
    *optr = '\0';
    return output_buff;
}

void trigger(const char *filtered_cmd) {
    pid_t pid;
    int fd[2];
    pipe(fd);
    fprintf(stderr, "[trigger] executing command: %s\n", filtered_cmd);
    if ((pid = fork()) == -1)
    {
        fprintf(stderr, "[trigger] failed to fork\n");
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
        if (idle_info == MPD_IDLE_PLAYER)
        {
            int et, tt;
            mpd_send_status(conn); 
            status = mpd_recv_status(conn);
            if (!status) MAINLOOP_ERROR;
            state = state_name[mpd_status_get_state(status)];
            fprintf(stderr, "[mpd] new event: %s\n", state);
            et = mpd_status_get_elapsed_time(status);
            tt = mpd_status_get_total_time(status);
            snprintf(etime_buff, sizeof(etime_buff), "%i", et);
            snprintf(ttime_buff, sizeof(ttime_buff), "%i", tt);
            snprintf(epct_buff, sizeof(epct_buff), "%d", tt ? et * 100 / tt : 0);
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

struct option long_options[] = {
    {"port", required_argument, NULL, 'p'},
    {"execute", required_argument, NULL, 'e'},
    {"help", no_argument, NULL, 'h'},
    {"shell", required_argument, NULL, 's'},
    {"retry", required_argument, NULL, 'r'},
    {0, 0, 0, 0}
};


int str_to_int(char *repr, int *flag) {
    char *endptr;
    int val = (int)strtol(repr, &endptr, 10);
    if (endptr == repr || endptr != repr + strlen(repr))
    {
        *flag = 0;
        return 0;
    }
    *flag = 1;
    return val;
}

int main(int argc, char **argv) {
    int opt, ind = 0;
    dict = hash_table_create();
    hash_table_register(dict, "title", &title);
    hash_table_register(dict, "artist", &artist);
    hash_table_register(dict, "album", &album);
    hash_table_register(dict, "track", &track);
    hash_table_register(dict, "state", &state);
    hash_table_register(dict, "elapsed_time", &elapsed_time);
    hash_table_register(dict, "total_time", &total_time);
    hash_table_register(dict, "elapsed_pct", &elapsed_pct);
    while ((opt = getopt_long(argc, argv, "p:e:s:r:h", long_options, &ind)) != -1)
    {
        int flag;
        switch (opt)
        {
            case 'p':
                port = str_to_int(optarg, &flag);
                if (!flag)
                    return fprintf(stderr, "Port number should be an integer.\n"), 1;
                break;
            case 'r':
                reconnect_time = str_to_int(optarg, &flag);
                if (!flag)
                    return fprintf(stderr, "reconnect time should be an integer.\n"), 1;
                break;
            case 'e':
                trigger_command = optarg;
                break;
            case 's':
                shell = optarg;
                break;
            case 'h':
                fprintf(stderr,
                       "mpd_trigger: Execute whatever you want when MPD "
                       "(Music Player Daemon) changes its state \n\n"
                       "Usage: mpd_trigger [OPTION]... [HOST]\n\n"
                       "  -p, --port\t\tspecify a port number (6600 by default)\n"
                       "  -e, --execute\t\tspecify the command to trigger "
                       "(special patterns are supported: {title} {track} ... {str1?str2:str3})\n"
                       "  -s, --shell\t\tspecify shell used to interpret the command\n"
                       "  -r, --retry\t\tspecify reconnect time (in seconds)\n"
                       "  -h, --help\t\tshow this info\n"
                       "\nAuthor: Ted Yin <ted.sybil@gmail.com>\n");
                return 0;
        }
    }
    if (optind < argc) host = argv[optind];
    for (;;)
    {
        fprintf(stderr, "[mpd] trying to connect %s:%d\n", host, port);
        conn = mpd_connection_new(host, port, 0);
        main_loop();
        fprintf(stderr, "[mpd] reconnecting\n");
        sleep(reconnect_time);
    }
    return 0;
}
