all: mpd_trigger
mpd_trigger: mpd_trigger.o
	gcc -o $@ $< -lmpdclient
mpd_trigger.o: mpd_trigger.c
	gcc -c $< -g
